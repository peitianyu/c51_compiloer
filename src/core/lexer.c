#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cc.h"

#define PP_LINE_SIZE 4096

#define make_null(x) make_token(TTYPE_NULL, (uintptr_t) 0)
#define make_strtok(x) make_token(TTYPE_STRING, (uintptr_t) get_cstring(x))
#define make_ident(x) make_token(TTYPE_IDENT, (uintptr_t) get_cstring(x))
#define make_punct(x) make_token(TTYPE_PUNCT, (uintptr_t)(x))
#define make_number(x) make_token(TTYPE_NUMBER, (uintptr_t)(x))
#define make_char(x) make_token(TTYPE_CHAR, (uintptr_t)(x))

static int ungotten_count = 0;
static Token ungotten_buf[16] = {{0}};

static int getc_with_pos(void);
static int ungetc_with_pos(int c);

// 位置跟踪变量
static int curr_line = 1;
static int curr_col = 1;
static const char *curr_filename = "<stdin>";
static TokenInfo curr_token_info = {0};

// 用于跟踪上一行的长度，以便正确回退
static int last_line_length = 0;
static int prev_col = 1;

static Token make_token(enum TokenType type, uintptr_t data)
{
    return (Token){
        .type = type,
        .priv = data,
    };
}

static int read_escaped_char(void)
{
    int c = getc_with_pos();
    if (c == EOF)
        return EOF;
    if (c != '\\')
        return c;

    c = getc_with_pos();
    if (c == EOF)
        return EOF;

    switch (c) {
    case '\'' : return '\'';
    case '"' : return '"';
    case 'n' : return '\n';
    case 'r' : return '\r';
    case 't' : return '\t';
    case '\\': return '\\';
    case 'a' : return '\a';
    case 'b' : return '\b';
    case 'f' : return '\f';
    case 'v' : return '\v';
    case '?':  return '?';
    case 'x': {
        /* 十六进制转义：\xNN */
        int val = 0;
        int nc = getc_with_pos();
        while (isxdigit(nc)) {
            val = val * 16 + (isdigit(nc) ? nc - '0' : (nc & 0xDF) - 'A' + 10);
            nc = getc_with_pos();
        }
        ungetc_with_pos(nc);
        return val & 0xFF;
    }
    case '0': case '1': case '2': case '3':
    case '4': case '5': case '6': case '7': {
        /* 八进制转义：\NNN（最多3位） */
        int val = c - '0';
        for (int i = 0; i < 2; i++) {
            int nc = getc_with_pos();
            if (nc >= '0' && nc <= '7')
                val = val * 8 + (nc - '0');
            else { ungetc_with_pos(nc); break; }
        }
        return val & 0xFF;
    }
    default:
        error("Unknown quote: %c", c);
    }
    return EOF;
}

static void update_pos(int c) {
    if (c == '\n') {
        curr_line++;
        last_line_length = curr_col;
        curr_col = 1;
    } else {
        curr_col++;
    }
}

static int getc_with_pos(void) {
    int c = getc(stdin);
    if (c != EOF) {
        update_pos(c);
    }
    return c;
}

static int ungetc_with_pos(int c) {
    if (c == '\n') {
        curr_line--;
        curr_col = last_line_length;
    } else if (c != EOF) {
        curr_col--;
    }
    return ungetc(c, stdin);
}

static const char *skip_space_str(const char *p)
{
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

static bool handle_line_directive(void)
{
    char buf[PP_LINE_SIZE];
    int i = 0;
    int c;

    while ((c = getc_with_pos()) != EOF && c != '\n') {
        if (i < (int)sizeof(buf) - 1) buf[i++] = (char)c;
    }
    buf[i] = '\0';

    const char *p = skip_space_str(buf);
    if (strncmp(p, "line", 4) == 0 && isspace((unsigned char)p[4])) {
        p += 4;
        p = skip_space_str(p);
    }

    if (!isdigit((unsigned char)*p)) return false;
    int line = (int)strtol(p, (char **)&p, 10);
    p = skip_space_str(p);

    if (*p == '"') {
        p++;
        const char *start = p;
        while (*p && *p != '"') p++;
        if (*p == '"') {
            size_t len = p - start;
            char *fname = malloc(len + 1);
            memcpy(fname, start, len);
            fname[len] = '\0';
            curr_filename = fname;
        }
    }

    curr_line = line;
    curr_col = 1;
    last_line_length = 0;
    return true;
}

static int getc_nonspace(void)
{
    int c;
    while ((c = getc_with_pos()) != EOF) {
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t')
            continue;
        return c;
    }
    return EOF;
}


static Token read_number(char first)
{
    String s = make_string();
    long long ival = 0, fval = 0;
    int  base = 10, dot = 0, fscale = 0;

    /* 0x... / 0b... 前缀 */
    if (first == '0') {
        int x = getc_with_pos();
        if (x == 'x' || x == 'X')        base = 16;
        else if (x == 'b' || x == 'B')   base = 2;
        else ungetc_with_pos(x);
    }

    /* 主循环：读整数/小数/十六进制位 */
    for (int c = first;; c = getc_with_pos()) {
        if (base == 16 && isxdigit(c))
            ival = ival * 16 + (isdigit(c) ? c - '0' : (c & 0xDF) - 'A' + 10);
        else if ((base == 2 && (c == '0' || c == '1')) ||
                 (base == 10 && isdigit(c))) {
            if (!dot) ival = ival * base + (c - '0');
            else      fval = fval * 10 + (c - '0'), ++fscale;
        }
        else if (base == 10 && c == '.' && !dot) { dot = 1; }
        else { ungetc_with_pos(c); break; }
    }

    /* 后缀：u/U, l/L, ll/LL, f/F 及其组合 (ul, lu, ull, llu ...) */
    int is_float = 0;
    for (;;) {
        int suf = getc_with_pos();
        if (suf == 'f' || suf == 'F') { is_float = 1; }
        else if (suf == 'l' || suf == 'L' || suf == 'u' || suf == 'U') { /* skip */ }
        else { ungetc_with_pos(suf); break; }
    }

    if (dot || is_float)
        string_appendf(&s, "%lld.%0*lld", ival, fscale, fval);
    else if (base != 10) {
        /* 对于十六进制/二进制常量，保留 32-bit 有符号位模式：
         * 0xffffffff -> -1 (int), 0x1ffffffff -> 8589934591 (long) */
        if (ival > (long long)0x7fffffffLL && ival <= (long long)0xffffffffLL)
            string_appendf(&s, "%d", (int)(unsigned int)ival);
        else
            string_appendf(&s, "%lld", ival);
    } else
        string_appendf(&s, "%lld", ival);

    return make_number(get_cstring(s));
}

static Token read_char(void)
{
    int c = read_escaped_char();
    if (c == EOF)
        goto err;
    int c2 = getc_with_pos();
    if (c2 == EOF)
        goto err;
    if (c2 != '\'')
        error("Malformed char literal");
    return make_char(c);
err:
    error("Unterminated char");
    return make_null(); /* non-reachable */
}

static Token read_string(void)
{
    String s = make_string();
    while (1) {
        int c = getc_with_pos();
        if (c == EOF)
            error("Unterminated string");
        if (c == '"')
            break;
        if (c == '\\') {
            ungetc_with_pos(c);
            c = read_escaped_char();
            if (c == EOF)
                error("Unterminated \\");
        }
        string_append(&s, c);
    }
    return make_strtok(s);
}

static Token read_ident(char c)
{
    String s = make_string();
    string_append(&s, c);
    while (1) {
        int c2 = getc_with_pos();
        if (isalnum(c2) || c2 == '_' || c2 >= 0x80) {
            string_append(&s, c2);
        } else {
            ungetc_with_pos(c2);

            /* L'x' 宽字符字面量 → 当作普通 char */
            const char *ident_str = get_cstring(s);
            if (!strcmp(ident_str, "L")) {
                int nc = getc_with_pos();
                if (nc == '\'') return read_char();
                if (nc == '"')  return read_string();
                ungetc_with_pos(nc);
            }

            if(!strcmp(ident_str, "true"))         return make_number("1\0");
            else if(!strcmp(ident_str, "false"))   return make_number("0\0");
            else                                   return make_ident(s);
        }
    }
}

static void skip_line_comment(void)
{
    while (1) {
        int c = getc_with_pos();
        if (c == '\n' || c == EOF)
            return;
    }
}

static void skip_block_comment(void)
{
    enum { in_comment, asterisk_read } state = in_comment;
    while (1) {
        int c = getc_with_pos();
        if (c == EOF) {
            error("Unterminated block comment");
            return;
        }
        
        if (state == in_comment) {
            if (c == '*')
                state = asterisk_read;
        } else if (state == asterisk_read) {
            if (c == '/') {
                return;
            } else if (c == '*') {
                continue;
            } else {
                state = in_comment;
            }
        }
    }
}

static Token read_rep(int expect, int t1, int t2)
{
    int c = getc_with_pos();
    if (c == expect)
        return make_punct(t2);
    ungetc_with_pos(c);
    return make_punct(t1);
}

static void update_token_info(int start_line, int start_col, Token *tok) {
    int token_len = curr_col - start_col;
    if (token_len < 0) {
        token_len = 1;
    } else if (token_len == 0) {
        token_len = 1;
    }
    
    curr_token_info = (TokenInfo){
        .file = curr_filename,
        .line = start_line,
        .col = start_col,
        .len = token_len
    };
}

static Token read_token_int(void)
{
    int c = getc_nonspace();
    int start_line = curr_line;
    int start_col = curr_col-1;

    Token tok;
    switch (c) {
    case '#':
        if (handle_line_directive())
            return read_token_int();
        error("invalid preprocessor directive");
        tok = make_null();
        update_token_info(start_line, start_col, &tok);
        return tok;
    case '0' ... '9':
        tok = read_number(c);
        update_token_info(start_line, start_col, &tok);
        return tok;
    case 'a' ... 'z':
    case 'A' ... 'Z':
    case '_':
        tok = read_ident(c);
        update_token_info(start_line, start_col, &tok);
        return tok;
    case '/': {
        c = getc_with_pos();
        if (c == '/') {
            skip_line_comment();
            return read_token_int();
        }
        if (c == '*') {
            skip_block_comment();
            return read_token_int();
        }
        if (c == '=') {
            tok = make_punct(PUNCT_DIV_ASSIGN);
            update_token_info(start_line, start_col, &tok);
            return tok;
        }
        ungetc_with_pos(c);
        tok = make_punct('/');
        update_token_info(start_line, start_col, &tok);
        return tok;
    }
    case '*': {
        c = getc_with_pos();
        if (c == '=') {
            tok = make_punct(PUNCT_MUL_ASSIGN);
            update_token_info(start_line, start_col, &tok);
            return tok;
        }
        ungetc_with_pos(c);
        tok = make_punct('*');
        update_token_info(start_line, start_col, &tok);
        return tok;
    }
    case '(':
    case ')':
    case ',':
    case ';':
    case '[':
    case ']':
    case '{':
    case '}':
    case '?':
    case ':':
        tok = make_punct(c);
        update_token_info(start_line, start_col, &tok);
        return tok;
    case '%': {
        c = getc_with_pos();
        if (c == '=') {
            tok = make_punct(PUNCT_MOD_ASSIGN);
            update_token_info(start_line, start_col, &tok);
            return tok;
        }
        ungetc_with_pos(c);
        tok = make_punct('%');
        update_token_info(start_line, start_col, &tok);
        return tok;
    }
    case '~':
        tok = make_punct('~');
        update_token_info(start_line, start_col, &tok);
        return tok;
    case '^': {
        c = getc_with_pos();
        if (c == '=') {
            tok = make_punct(PUNCT_XOR_ASSIGN);
            update_token_info(start_line, start_col, &tok);
            return tok;
        }
        ungetc_with_pos(c);
        tok = make_punct('^');
        update_token_info(start_line, start_col, &tok);
        return tok;
    }
    case '-':
        c = getc_with_pos();
        if (c == '-') {
            tok = make_punct(PUNCT_DEC);
            update_token_info(start_line, start_col, &tok);
            return tok;
        }
        if (c == '>') {
            tok = make_punct(PUNCT_ARROW);
            update_token_info(start_line, start_col, &tok);
            return tok;
        }
        if (c == '=') {
            tok = make_punct(PUNCT_SUB_ASSIGN);
            update_token_info(start_line, start_col, &tok);
            return tok;
        }
        ungetc_with_pos(c);
        tok = make_punct('-');
        update_token_info(start_line, start_col, &tok);
        return tok;
    case '=':
        tok = read_rep('=', '=', PUNCT_EQ);
        update_token_info(start_line, start_col, &tok);
        return tok;
    case '+': {
        c = getc_with_pos();
        if (c == '+') {
            tok = make_punct(PUNCT_INC);
            update_token_info(start_line, start_col, &tok);
            return tok;
        }
        if (c == '=') {
            tok = make_punct(PUNCT_ADD_ASSIGN);
            update_token_info(start_line, start_col, &tok);
            return tok;
        }
        ungetc_with_pos(c);
        tok = make_punct('+');
        update_token_info(start_line, start_col, &tok);
        return tok;
    }
    case '.':
        c = getc_with_pos();
        if(c != '.') {
            ungetc_with_pos(c);
            tok = make_punct('.');
            update_token_info(start_line, start_col, &tok);
            return tok; 
        } else {
            tok = read_rep('.', '.', PUNCT_ELLIPSIS);
            update_token_info(start_line, start_col, &tok);
            return tok;
        }
    case '!':
        c = getc_with_pos();
        if(c == '=') {
            tok = make_punct(PUNCT_NE);
            update_token_info(start_line, start_col, &tok);
            return tok;
        } else {
            ungetc_with_pos(c);
            tok = make_punct('!');
            update_token_info(start_line, start_col, &tok);
            return tok;
        }
    case '&': {
        c = getc_with_pos();
        if (c == '&') {
            tok = make_punct(PUNCT_LOGAND);
            update_token_info(start_line, start_col, &tok);
            return tok;
        }
        if (c == '=') {
            tok = make_punct(PUNCT_AND_ASSIGN);
            update_token_info(start_line, start_col, &tok);
            return tok;
        }
        ungetc_with_pos(c);
        tok = make_punct('&');
        update_token_info(start_line, start_col, &tok);
        return tok;
    }
    case '|': {
        c = getc_with_pos();
        if (c == '|') {
            tok = make_punct(PUNCT_LOGOR);
            update_token_info(start_line, start_col, &tok);
            return tok;
        }
        if (c == '=') {
            tok = make_punct(PUNCT_OR_ASSIGN);
            update_token_info(start_line, start_col, &tok);
            return tok;
        }
        ungetc_with_pos(c);
        tok = make_punct('|');
        update_token_info(start_line, start_col, &tok);
        return tok;
    }
    case '<':
        c = getc_with_pos();
        if(c == '=') {
            tok = make_punct(PUNCT_LE);
            update_token_info(start_line, start_col, &tok);
            return tok;
        } else if (c == '<') {
            int d = getc_with_pos();
            if (d == '=') {
                tok = make_punct(PUNCT_SHL_ASSIGN);
                update_token_info(start_line, start_col, &tok);
                return tok;
            }
            ungetc_with_pos(d);
            tok = make_punct(PUNCT_LSHIFT);
            update_token_info(start_line, start_col, &tok);
            return tok;
        } else {
            ungetc_with_pos(c);
            tok = make_punct('<');
            update_token_info(start_line, start_col, &tok);
            return tok;
        }
    case '>':
        c = getc_with_pos();
        if(c == '=') {
            tok = make_punct(PUNCT_GE);
            update_token_info(start_line, start_col, &tok);
            return tok;
        } else if (c == '>') {
            int d = getc_with_pos();
            if (d == '=') {
                tok = make_punct(PUNCT_SHR_ASSIGN);
                update_token_info(start_line, start_col, &tok);
                return tok;
            }
            ungetc_with_pos(d);
            tok = make_punct(PUNCT_RSHIFT);
            update_token_info(start_line, start_col, &tok);
            return tok;
        } else {
            ungetc_with_pos(c);
            tok = make_punct('>');
            update_token_info(start_line, start_col, &tok);
            return tok;
        }
    case '"':
        tok = read_string();
        update_token_info(start_line, start_col, &tok);
        return tok;
    case '\'':
        tok = read_char();
        update_token_info(start_line, start_col, &tok);
        return tok;
    case EOF:
        tok = make_null();
        update_token_info(start_line, start_col, &tok);
        return tok;
    default:
        if ((unsigned char)c >= 0x80) {
            tok = read_ident(c);
            update_token_info(start_line, start_col, &tok);
            return tok;
        }
        error("Unexpected character: '%c'", c);
        tok = make_null(); /* non-reachable */
        update_token_info(start_line, start_col, &tok);
        return tok;
    }
}

bool is_punct(const Token tok, int c)
{
    return (get_ttype(tok) == TTYPE_PUNCT) && (get_punct(tok) == c);
}

void unget_token(const Token tok)
{
    if (get_ttype(tok) == TTYPE_NULL)
        return;
    if (ungotten_count >= 16)
        error("Push back buffer is already full");
    ungotten_buf[ungotten_count++] = make_token(tok.type, tok.priv);
}

Token peek_token(void)
{
    Token tok = read_token();
    unget_token(tok);
    return tok;
}

Token read_token(void)
{
    if (ungotten_count > 0) {
        int idx = --ungotten_count;
        return make_token(ungotten_buf[idx].type, ungotten_buf[idx].priv);
    }
    return read_token_int();
}

TokenInfo get_current_token_info(void) {
    return curr_token_info;
}

void set_current_filename(const char *filename) {
    curr_filename = filename;
    curr_line = 1;
    curr_col = 1;
    last_line_length = 0;
}