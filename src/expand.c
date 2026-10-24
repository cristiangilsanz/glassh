#include <ctype.h>
#include <glob.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "glassh.h"

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} Buffer;

typedef struct {
    Buffer value;
    Buffer pattern;
    bool quoted;
    bool globbing;
} Word;

static void put(Buffer *buffer, char c)
{
    if (buffer->length + 2 > buffer->capacity) {
        buffer->capacity = buffer->capacity ? buffer->capacity * 2 : 32;
        buffer->data = checked_realloc(buffer->data, buffer->capacity);
    }
    buffer->data[buffer->length++] = c;
    buffer->data[buffer->length] = '\0';
}

static void literal(Word *word, char c)
{
    put(&word->value, c);
    if (strchr("*?[\\", c)) {
        put(&word->pattern, '\\');
    }
    put(&word->pattern, c);
}

static void literal_text(Word *word, const char *text)
{
    for (; *text; text++) {
        literal(word, *text);
    }
}

static bool is_name_start(char c)
{
    return isalpha((unsigned char)c) || c == '_';
}

static bool is_name_char(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

bool is_assignment(const char *raw)
{
    if (!is_name_start(raw[0])) {
        return false;
    }
    size_t i = 1;
    while (is_name_char(raw[i])) {
        i++;
    }
    return raw[i] == '=';
}

static size_t variable(Shell *shell, Word *word, const char *at, char **error)
{
    char number[32];
    if (at[1] == '?') {
        snprintf(number, sizeof number, "%d", shell->status);
        literal_text(word, number);
        return 2;
    }
    if (at[1] == '$') {
        snprintf(number, sizeof number, "%ld", (long)getpid());
        literal_text(word, number);
        return 2;
    }
    size_t start = 1;
    size_t end = 1;
    bool braced = at[1] == '{';
    if (braced) {
        start = end = 2;
        while (at[end] && at[end] != '}') {
            end++;
        }
        if (!at[end] || end == start) {
            *error = checked_strdup("bad substitution");
            return 0;
        }
    } else {
        if (!is_name_start(at[1])) {
            literal(word, '$');
            return 1;
        }
        while (is_name_char(at[end])) {
            end++;
        }
    }
    char *name = checked_malloc(end - start + 1);
    memcpy(name, at + start, end - start);
    name[end - start] = '\0';
    const char *value = getenv(name);
    free(name);
    if (value) {
        literal_text(word, value);
    }
    return end + (braced ? 1 : 0);
}

static bool expand_word(Shell *shell, const char *raw, Word *word, char **error)
{
    *word = (Word){0};
    put(&word->value, '\0');
    word->value.length = 0;
    put(&word->pattern, '\0');
    word->pattern.length = 0;
    size_t i = 0;
    if (raw[0] == '~' && (raw[1] == '/' || raw[1] == '\0')) {
        const char *home = getenv("HOME");
        literal_text(word, home ? home : "~");
        i = 1;
    }
    while (raw[i]) {
        char c = raw[i];
        if (c == '\'') {
            word->quoted = true;
            for (i++; raw[i] && raw[i] != '\''; i++) {
                literal(word, raw[i]);
            }
            i++;
        } else if (c == '"') {
            word->quoted = true;
            for (i++; raw[i] && raw[i] != '"';) {
                if (raw[i] == '\\' && raw[i + 1] && strchr("$\"\\`", raw[i + 1])) {
                    literal(word, raw[i + 1]);
                    i += 2;
                } else if (raw[i] == '$') {
                    size_t used = variable(shell, word, raw + i, error);
                    if (*error) {
                        return false;
                    }
                    i += used;
                } else {
                    literal(word, raw[i++]);
                }
            }
            i++;
        } else if (c == '\\') {
            word->quoted = true;
            literal(word, raw[i + 1] ? raw[i + 1] : '\\');
            i += raw[i + 1] ? 2 : 1;
        } else if (c == '$') {
            size_t used = variable(shell, word, raw + i, error);
            if (*error) {
                return false;
            }
            i += used;
        } else {
            if (strchr("*?[", c)) {
                word->globbing = true;
                put(&word->value, c);
                put(&word->pattern, c);
            } else {
                literal(word, c);
            }
            i++;
        }
    }
    return true;
}

static void push(char ***words, size_t *count, const char *text)
{
    *words = checked_realloc(*words, (*count + 2) * sizeof(char *));
    (*words)[(*count)++] = checked_strdup(text);
    (*words)[*count] = NULL;
}

char **expand_words(Shell *shell, char *const *raw, size_t count, size_t *expanded, char **error)
{
    *error = NULL;
    char **words = checked_calloc(1, sizeof(char *));
    *expanded = 0;
    for (size_t i = 0; i < count; i++) {
        Word word;
        if (!expand_word(shell, raw[i], &word, error)) {
            free(word.value.data);
            free(word.pattern.data);
            free_words(words);
            return NULL;
        }
        glob_t matches;
        if (word.globbing && glob(word.pattern.data, 0, NULL, &matches) == 0) {
            for (size_t m = 0; m < matches.gl_pathc; m++) {
                push(&words, expanded, matches.gl_pathv[m]);
            }
            globfree(&matches);
        } else if (word.quoted || word.value.length) {
            push(&words, expanded, word.value.data);
        }
        free(word.value.data);
        free(word.pattern.data);
    }
    return words;
}

char *expand_single(Shell *shell, const char *raw, char **error)
{
    size_t count;
    char *const words[] = {(char *)raw};
    char **expanded = expand_words(shell, words, 1, &count, error);
    if (!expanded) {
        return NULL;
    }
    if (count != 1) {
        free_words(expanded);
        size_t size = strlen(raw) + 32;
        *error = checked_malloc(size);
        snprintf(*error, size, "%s: ambiguous redirect", raw);
        return NULL;
    }
    char *result = expanded[0];
    free(expanded);
    return result;
}

void free_words(char **words)
{
    if (!words) {
        return;
    }
    for (char **word = words; *word; word++) {
        free(*word);
    }
    free(words);
}
