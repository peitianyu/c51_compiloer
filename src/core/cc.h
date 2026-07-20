#ifndef CC_H
#define CC_H

#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include "dict.h"
#include "list.h"
#include "platform.h"

enum TokenType {
    TTYPE_NULL,
    TTYPE_IDENT,
    TTYPE_PUNCT,
    TTYPE_NUMBER,
    TTYPE_CHAR,
    TTYPE_STRING,
};

typedef struct {
    int type;
    uintptr_t priv;
} Token;

typedef struct {
    const char *file;
    int line, col, len;
} TokenInfo;

extern TokenInfo get_current_token_info(void);
#include "util.h"

enum {
    AST_LITERAL = 256,
    AST_STRING,
    AST_LVAR,
    AST_GVAR,
    AST_FUNCALL,
    AST_FUNC_DECL,
    AST_FUNC_DEF,
    AST_DECL,
    AST_ARRAY_INIT,
    AST_ADDR,
    AST_DEREF,
    AST_IF,
    AST_TERNARY,
    AST_FOR,
    AST_WHILE,
    AST_DO_WHILE,
    AST_LABEL,
    AST_GOTO,
    AST_BREAK,
    AST_CONTINUE,
    AST_RETURN,
    AST_SWITCH,
    AST_COMPOUND_STMT,
    AST_STRUCT_REF,
    AST_BIT_REF,
    AST_STRUCT_DEF,
    AST_STRUCT_INIT,
    AST_ENUM_DEF,
    AST_TYPE_DEF,
    AST_CAST,
    AST_POST_INC,
    AST_POST_DEC,
    AST_ASM,
    AST_STATIC_ASSERT,
    PUNCT_EQ,
    PUNCT_GE,        // >=
    PUNCT_LE,        // <=
    PUNCT_NE,        // !=
    PUNCT_ELLIPSIS,
    PUNCT_INC,
    PUNCT_DEC,
    PUNCT_LOGAND,
    PUNCT_LOGOR,
    PUNCT_ARROW,
    PUNCT_LSHIFT,
    PUNCT_RSHIFT,

    /* compound assignment operators */
    PUNCT_SHL_ASSIGN,  /* <<= */
    PUNCT_SHR_ASSIGN,  /* >>= */
    PUNCT_AND_ASSIGN,  /* &= */
    PUNCT_OR_ASSIGN,   /* |= */
    PUNCT_XOR_ASSIGN,  /* ^= */
    PUNCT_ADD_ASSIGN,  /* += */
    PUNCT_SUB_ASSIGN,  /* -= */
    PUNCT_MUL_ASSIGN,  /* *= */
    PUNCT_DIV_ASSIGN,  /* /= */
    PUNCT_MOD_ASSIGN,  /* %= */
};

enum {
    CTYPE_VOID,
    CTYPE_BOOL,
    CTYPE_CHAR,
    CTYPE_INT,
    CTYPE_LONG,
    CTYPE_FLOAT,
    CTYPE_DOUBLE,
    CTYPE_ARRAY,
    CTYPE_PTR,
    CTYPE_STRUCT,
    CTYPE_ENUM
};

typedef struct __CtypeAttr {
    int ctype_const         : 1;
    int ctype_volatile      : 1;
    int ctype_restrict      : 1;
    int ctype_static        : 1;
    int ctype_extern        : 1;
    int ctype_unsigned      : 1;
    int ctype_register      : 1;
    int ctype_typedef       : 1;

    /* 函数限定 */
    int ctype_inline        : 1;
    int ctype_noreturn      : 1;

    /* C51 特殊基础类型标签（用于保留语义到输出层） */
    int ctype_c51_sfr       : 1;
    int ctype_c51_sfr16     : 1;
    int ctype_c51_sbit      : 1;
    int ctype_c51_bit       : 1;

    /* C51 内存区/地址空间标签 */
    int ctype_c51_data      : 1;
    int ctype_c51_idata     : 1;
    int ctype_c51_xdata     : 1;
    int ctype_c51_pdata     : 1;
    int ctype_c51_code      : 1;
    int ctype_c51_bdata     : 1;
    int ctype_c51_near      : 1;
    int ctype_c51_far       : 1;
} CtypeAttr;

typedef struct __Ctype {
    int attr;
    int type;
    int size;
    struct __Ctype *ptr; /* pointer or array */
    int len;             /* array length */
    /* struct */
    Dict *fields;
    int offset;
    bool is_union;

    int bit_offset;     /* 位域支持 */
    int bit_size;
    char *tag;          /* struct/union/enum 标签名，用于打印时避免无限递归 */
} Ctype;

typedef struct __Ast {
    int type;
    Ctype *ctype;
    const char *source_file;  /* 声明来源文件路径，用于 codegen 按文件分流 */
    union {
        /* char, int, or long */
        long long ival;

        /* float or double */
        struct {
            union {
                double fval;
                int lval[2];
            };
            char *flabel;
        };

        /* string literal */
        struct {
            char *sval;
            char *slabel;
        };

        /* Local/global variable */
        struct {
            char *varname;
            struct {
                int loff;
                char *glabel;
                struct __Ast *ginit; /* store global initializer AST if available */
            };
        };

        /* Binary operator */
        struct {
            struct __Ast *left;
            struct __Ast *right;
        };

        /* Unary operator */
        struct {
            struct __Ast *operand;
        };

        /* Function call or function declaration */
        struct {
            char *fname;
            struct {
                List *args;
                struct __Ast *fnexpr;
                struct {
                    List *params;
                    List *localvars;
                    List *labels;
                    struct __Ast *body;
                };
            };
        };

        /* Declaration */
        struct {
            struct __Ast *declvar;
            struct __Ast *declinit;
        };

        /* Array initializer */
        List *arrayinit;

        /* Struct initializer */
        List *structinit; 

        /* Typedef name */
        char* typename;

        /* if statement or ternary operator */
        struct {
            struct __Ast *cond;
            struct __Ast *then;
            struct __Ast *els;
        };

        /* for statement */
        struct {
            struct __Ast *forinit;
            struct __Ast *forcond;
            struct __Ast *forstep;
            struct __Ast *forbody;
        };

        /* while/do-while statement */
        struct {
            struct __Ast *while_cond;
            struct __Ast *while_body;
        };

        /* switch-case statement */
        struct {
            struct __Ast *ctrl;         
            List *cases;       /* List<SwitchCase*> */
            struct __Ast *default_stmt;
            struct __Ast *switch_body;
            char *default_label;
        };

        /* goto/label */
        char* label;

        /* inline asm body */
        char *asm_body;

        /* return statement */
        struct __Ast *retval;

        /* Compound statement */
        List *stmts;

        /* Struct reference / Bit reference */
        struct {
            struct __Ast *struc;
            char *field; /* specific to ast_to_string only */
            int bit_index; /* for AST_BIT_REF */
        };

        /* cast */
        struct {
            struct __Ast *cast_expr;   
        };
    };

    /* 平台相关函数修饰信息（仅函数声明/定义使用）
     * 通过 platform.h 中的 PlatInfo 联合体统一管理，
     * 按当前目标平台（g_target_platform）解读对应成员。 */
    PlatInfo platform_info;
} Ast;

typedef struct {
    long low, high;
    Ast *stmt;
    char *label;
} SwitchCase;

/* verbose.c */
extern char *token_to_string(const Token tok);
extern char *ast_to_string(Ast *ast);
extern char *ctype_to_string(Ctype *ctype);

/* sexpr.c */
extern char *ast_to_sexpr(Ast *ast);

/* lexer.c */
extern bool is_punct(const Token tok, int c);
extern void unget_token(const Token tok);
extern Token peek_token(void);
extern Token read_token(void);
extern TokenInfo get_current_token_info(void) __attribute__((__unused__));
extern void set_current_filename(const char *filename);

#define get_priv(tok, type)                                       \
    ({                                                            \
        assert(__builtin_types_compatible_p(typeof(tok), Token)); \
        ((type) tok.priv);                                        \
    })

#define get_ttype(tok)                                            \
    ({                                                            \
        assert(__builtin_types_compatible_p(typeof(tok), Token)); \
        (tok.type);                                               \
    })

#define get_token(tok, ttype, priv_type) \
    ({                                   \
        assert(get_ttype(tok) == ttype); \
        get_priv(tok, priv_type);        \
    })

#define get_char(tok)   get_token(tok, TTYPE_CHAR, char)
#define get_strtok(tok) get_token(tok, TTYPE_STRING, char *)
#define get_ident(tok)  get_token(tok, TTYPE_IDENT, char *)
#define get_number(tok) get_token(tok, TTYPE_NUMBER, char *)
#define get_punct(tok)  get_token(tok, TTYPE_PUNCT, int)

/* parser.c */
extern List *strings;
extern List *flonums;
extern List *ctypes;
extern void parser_reset(void);
extern void parser_set_target_mcs51(void);
extern char *make_label(void);
extern List *read_toplevels(void);
extern bool is_inttype(Ctype *ctype);
extern bool is_flotype(Ctype *ctype);
extern CtypeAttr get_attr(int in_attr);

/* pp.c */
extern bool pp_preprocess_to_stdin(const char *filename);
extern void pp_global_on_init(void (*hook)(void));
extern void pp_cleanup_temp(void);
extern void pp_global_add_include_path(const char *path);
extern void pp_global_define(const char *name, const char *body);

#endif /* CC_H */

