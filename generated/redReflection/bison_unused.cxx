/* A Bison parser, made by GNU Bison 3.8.2.  */

/* Bison implementation for Yacc-like parsers in C

   Copyright (C) 1984, 1989-1990, 2000-2015, 2018-2021 Free Software Foundation,
   Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.  */

/* As a special exception, you may create a larger work that contains
   part or all of the Bison parser skeleton and distribute that work
   under terms of your choice, so long as that work isn't itself a
   parser generator using the skeleton or a modified version thereof
   as a parser skeleton.  Alternatively, if you modify or redistribute
   the parser skeleton itself, you may (at your option) remove this
   special exception, which will cause the skeleton and the resulting
   Bison output files to be licensed under the GNU General Public
   License without this special exception.

   This special exception was added by the Free Software Foundation in
   version 2.2 of Bison.  */

/* C LALR(1) parser skeleton written by Richard Stallman, by
   simplifying the original so-called "semantic" parser.  */

/* DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
   especially those whose name start with YY_ or yy_.  They are
   private implementation details that can be changed or removed.  */

/* All symbols defined below should begin with yy or YY, to avoid
   infringing on user name space.  This should be done even for local
   variables, as they might otherwise be expanded by user macros.
   There are some unavoidable exceptions within include files to
   define necessary library symbols; they are noted "INFRINGES ON
   USER NAME SPACE" below.  */

/* Identify Bison output, and Bison version.  */
#define YYBISON 30802

/* Bison version string.  */
#define YYBISON_VERSION "3.8.2"

/* Skeleton name.  */
#define YYSKELETON_NAME "yacc.c"

/* Pure parsers.  */
#define YYPURE 1

/* Push parsers.  */
#define YYPUSH 0

/* Pull parsers.  */
#define YYPULL 1




/* First part of user prologue.  */
#line 1 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"

// Some yacc (bison) defines
#define YYDEBUG 1
#define YYERROR_VERBOSE 1

/// Disable warnings caused by Bison (disable these when editing the grammar to make sure you don't cause any yourself)
RED_DISABLE_WARNING_MSC( 4244 )	// conversion from 'int' to 'short', possible loss of data
RED_DISABLE_WARNING_MSC( 4065 )	// switch statement contains 'default' but no 'case' labels
RED_DISABLE_WARNING_MSC( 4267 )	// conversion from 'size_t' to 'int', possible loss of data
RED_DISABLE_WARNING_MSC( 4127 )	// conditional expression is constant
RED_DISABLE_WARNING_MSC( 4702 )	// Unreachable code

/// The parsing token
#define YYSTYPE YYSTYPE_File
#include "scriptFileParser_bison.cxx.h"

/// Parser interface
class CScriptFileParserWorker
{
public:
	CScriptFileParserWorker( CScriptTokenStream& tokens, CScriptFileParser& parser )
		: m_tokens( &tokens )
		, m_parser( &parser )
		, m_token( nullptr )
		, m_flags( 0 )
	{};

	int yylex( YYSTYPE* lvalp )
	{
		if( m_tokens->IsEndOfStream() )
			return 0;
	
		CScriptToken& token = m_tokens->GetReadToken();
		m_tokens->IncrementReadPosition();

		lvalp->m_context = &token.m_context;
		lvalp->m_token = token.m_text;

		m_token = &token;

		return token.m_token;
	}

	int yyerror( const char *msg, YYSTYPE* lvalp )
	{
		// Emit error
		const String& errorStringAnsi = String::Printf( "%hs, near '%hs'", msg, lvalp->m_token.ToString().AsChar() );
		m_parser->EmitError( lvalp->m_context, errorStringAnsi );

		// Continue
		return 0;
	}

	void ExtractFunctionCode()
	{
		m_tokens->ExtractFunctionTokens( m_parser->GetCodeTokens() );
	}

	void ExtractInitCode( CScriptTokenStream& initCode )
	{
		m_tokens->ExtractInitCode( initCode );
	}

	void SetGlobalFlags( const Uint64 flags )
	{
		m_flags = flags;
	}

	const Uint64 GetGlobalFlags() const
	{
		return m_flags;
	}

	void AddAttribute( const ScriptAttribute& attribute )
	{
		m_attributes.PushBack( attribute );
	}

	void ClearAttributes()
	{
		m_attributes.Clear();
	}

	const ScriptAttributes& GetAttributes() const
	{
		return m_attributes;
	}

	CScriptFileParser* GetParser()
	{
		return m_parser;
	}

	Int32 GetCurrentTokenLine() const
	{
		if( !m_token )
			return -1;

		return m_token->m_context.m_line;
	}

	// returns the line of the last token in current context
	int GetLastTokenLine() const
	{
		if ( m_tokens->IsEmpty() )
		{
			return -1;
		}

		return m_tokens->GetLastToken().m_context.m_line;
	}

private:
	CScriptTokenStream*		m_tokens;
	CScriptFileParser*		m_parser;
	CScriptToken*			m_token;

	ScriptAttributes		m_attributes;
	Uint64					m_flags;
};

// Error function
static int yyerror_thread_safe( const char *msg, void* param, YYSTYPE* lvalp )
{
	CScriptFileParserWorker* worker = (CScriptFileParserWorker*) param;
	return worker->yyerror( msg, lvalp );
}

// Token reader
int yylex_thread_safe( YYSTYPE* lvalp, void* param )
{
	CScriptFileParserWorker* worker = (CScriptFileParserWorker*) param;
	return worker->yylex( lvalp );	
}

#define GFileContext ((CScriptFileParserWorker*) param)
#define GFileParser (((CScriptFileParserWorker*) param)->GetParser())

#define YYPARSE_PARAM param 
#define YYLEX_PARAM param 

#define YYLEX yylex_thread_safe(&yylval, YYLEX_PARAM)
#define yyerror(msg) yyerror_thread_safe(msg, YYPARSE_PARAM, &yylval) 


#line 217 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"

# ifndef YY_CAST
#  ifdef __cplusplus
#   define YY_CAST(Type, Val) static_cast<Type> (Val)
#   define YY_REINTERPRET_CAST(Type, Val) reinterpret_cast<Type> (Val)
#  else
#   define YY_CAST(Type, Val) ((Type) (Val))
#   define YY_REINTERPRET_CAST(Type, Val) ((Type) (Val))
#  endif
# endif
# ifndef YY_NULLPTR
#  if defined __cplusplus
#   if 201103L <= __cplusplus
#    define YY_NULLPTR nullptr
#   else
#    define YY_NULLPTR 0
#   endif
#  else
#   define YY_NULLPTR ((void*)0)
#  endif
# endif

#include "bison_tokens.h"
/* Symbol kind.  */
enum yysymbol_kind_t
{
  YYSYMBOL_YYEMPTY = -2,
  YYSYMBOL_YYEOF = 0,                      /* "end of file"  */
  YYSYMBOL_YYerror = 1,                    /* error  */
  YYSYMBOL_YYUNDEF = 2,                    /* "invalid token"  */
  YYSYMBOL_TOKEN_IDENT = 3,                /* TOKEN_IDENT  */
  YYSYMBOL_TOKEN_INTEGER = 4,              /* TOKEN_INTEGER  */
  YYSYMBOL_TOKEN_FLOAT = 5,                /* TOKEN_FLOAT  */
  YYSYMBOL_TOKEN_NAME = 6,                 /* TOKEN_NAME  */
  YYSYMBOL_TOKEN_STRING = 7,               /* TOKEN_STRING  */
  YYSYMBOL_TOKEN_TDBID = 8,                /* TOKEN_TDBID  */
  YYSYMBOL_TOKEN_RESREF = 9,               /* TOKEN_RESREF  */
  YYSYMBOL_TOKEN_BOOL_TRUE = 10,           /* TOKEN_BOOL_TRUE  */
  YYSYMBOL_TOKEN_BOOL_FALSE = 11,          /* TOKEN_BOOL_FALSE  */
  YYSYMBOL_TOKEN_NULL = 12,                /* TOKEN_NULL  */
  YYSYMBOL_TOKEN_IMPORT = 13,              /* TOKEN_IMPORT  */
  YYSYMBOL_TOKEN_IMPORTONLY = 14,          /* TOKEN_IMPORTONLY  */
  YYSYMBOL_TOKEN_HOLY = 15,                /* TOKEN_HOLY  */
  YYSYMBOL_TOKEN_ABSTRACT = 16,            /* TOKEN_ABSTRACT  */
  YYSYMBOL_TOKEN_CONST = 17,               /* TOKEN_CONST  */
  YYSYMBOL_TOKEN_MUTABLE = 18,             /* TOKEN_MUTABLE  */
  YYSYMBOL_TOKEN_EXTENDS = 19,             /* TOKEN_EXTENDS  */
  YYSYMBOL_TOKEN_CLASS = 20,               /* TOKEN_CLASS  */
  YYSYMBOL_TOKEN_ENUM = 21,                /* TOKEN_ENUM  */
  YYSYMBOL_TOKEN_STRUCT = 22,              /* TOKEN_STRUCT  */
  YYSYMBOL_TOKEN_FUNCTION = 23,            /* TOKEN_FUNCTION  */
  YYSYMBOL_TOKEN_DEF = 24,                 /* TOKEN_DEF  */
  YYSYMBOL_TOKEN_EDITABLE = 25,            /* TOKEN_EDITABLE  */
  YYSYMBOL_TOKEN_INSTANCE_EDITABLE = 26,   /* TOKEN_INSTANCE_EDITABLE  */
  YYSYMBOL_TOKEN_REPLICATED = 27,          /* TOKEN_REPLICATED  */
  YYSYMBOL_TOKEN_PERSISTENT = 28,          /* TOKEN_PERSISTENT  */
  YYSYMBOL_TOKEN_FINAL = 29,               /* TOKEN_FINAL  */
  YYSYMBOL_TOKEN_VIRTUAL = 30,             /* TOKEN_VIRTUAL  */
  YYSYMBOL_TOKEN_OVERRIDE = 31,            /* TOKEN_OVERRIDE  */
  YYSYMBOL_TOKEN_OUT = 32,                 /* TOKEN_OUT  */
  YYSYMBOL_TOKEN_OPTIONAL = 33,            /* TOKEN_OPTIONAL  */
  YYSYMBOL_TOKEN_SKIP = 34,                /* TOKEN_SKIP  */
  YYSYMBOL_TOKEN_INLINED = 35,             /* TOKEN_INLINED  */
  YYSYMBOL_TOKEN_ARRAY = 36,               /* TOKEN_ARRAY  */
  YYSYMBOL_TOKEN_HINT = 37,                /* TOKEN_HINT  */
  YYSYMBOL_TOKEN_HINTS = 38,               /* TOKEN_HINTS  */
  YYSYMBOL_TOKEN_DEFAULT = 39,             /* TOKEN_DEFAULT  */
  YYSYMBOL_TOKEN_DEFAULTS = 40,            /* TOKEN_DEFAULTS  */
  YYSYMBOL_TOKEN_BROWSABLE = 41,           /* TOKEN_BROWSABLE  */
  YYSYMBOL_TOKEN_VAR = 42,                 /* TOKEN_VAR  */
  YYSYMBOL_TOKEN_EXEC = 43,                /* TOKEN_EXEC  */
  YYSYMBOL_TOKEN_TIMER = 44,               /* TOKEN_TIMER  */
  YYSYMBOL_TOKEN_WEAK = 45,                /* TOKEN_WEAK  */
  YYSYMBOL_TOKEN_OPERATOR = 46,            /* TOKEN_OPERATOR  */
  YYSYMBOL_TOKEN_CAST = 47,                /* TOKEN_CAST  */
  YYSYMBOL_TOKEN_IMPLICIT = 48,            /* TOKEN_IMPLICIT  */
  YYSYMBOL_TOKEN_STATIC = 49,              /* TOKEN_STATIC  */
  YYSYMBOL_TOKEN_VISUAL = 50,              /* TOKEN_VISUAL  */
  YYSYMBOL_TOKEN_CORE = 51,                /* TOKEN_CORE  */
  YYSYMBOL_TOKEN_LOGIC = 52,               /* TOKEN_LOGIC  */
  YYSYMBOL_TOKEN_MULTICAST = 53,           /* TOKEN_MULTICAST  */
  YYSYMBOL_TOKEN_HOST = 54,                /* TOKEN_HOST  */
  YYSYMBOL_TOKEN_CLIENT = 55,              /* TOKEN_CLIENT  */
  YYSYMBOL_TOKEN_RELIABLE = 56,            /* TOKEN_RELIABLE  */
  YYSYMBOL_TOKEN_QUEST = 57,               /* TOKEN_QUEST  */
  YYSYMBOL_TOKEN_REF = 58,                 /* TOKEN_REF  */
  YYSYMBOL_TOKEN_DEBUG = 59,               /* TOKEN_DEBUG  */
  YYSYMBOL_TOKEN_SYNCHRONIZED = 60,        /* TOKEN_SYNCHRONIZED  */
  YYSYMBOL_TOKEN_SIMPLE_TYPE = 61,         /* TOKEN_SIMPLE_TYPE  */
  YYSYMBOL_TOKEN_CLASS_TYPE = 62,          /* TOKEN_CLASS_TYPE  */
  YYSYMBOL_TOKEN_ENUM_TYPE = 63,           /* TOKEN_ENUM_TYPE  */
  YYSYMBOL_TOKEN_OP_IADD = 64,             /* TOKEN_OP_IADD  */
  YYSYMBOL_TOKEN_OP_ISUB = 65,             /* TOKEN_OP_ISUB  */
  YYSYMBOL_TOKEN_OP_IMUL = 66,             /* TOKEN_OP_IMUL  */
  YYSYMBOL_TOKEN_OP_IDIV = 67,             /* TOKEN_OP_IDIV  */
  YYSYMBOL_TOKEN_OP_IAND = 68,             /* TOKEN_OP_IAND  */
  YYSYMBOL_TOKEN_OP_IOR = 69,              /* TOKEN_OP_IOR  */
  YYSYMBOL_TOKEN_OP_LOGIC_OR = 70,         /* TOKEN_OP_LOGIC_OR  */
  YYSYMBOL_TOKEN_OP_LOGIC_AND = 71,        /* TOKEN_OP_LOGIC_AND  */
  YYSYMBOL_TOKEN_OP_EQUAL = 72,            /* TOKEN_OP_EQUAL  */
  YYSYMBOL_TOKEN_OP_NOTEQUAL = 73,         /* TOKEN_OP_NOTEQUAL  */
  YYSYMBOL_TOKEN_OP_GREQ = 74,             /* TOKEN_OP_GREQ  */
  YYSYMBOL_TOKEN_OP_LEEQ = 75,             /* TOKEN_OP_LEEQ  */
  YYSYMBOL_TOKEN_NEW = 76,                 /* TOKEN_NEW  */
  YYSYMBOL_TOKEN_IF = 77,                  /* TOKEN_IF  */
  YYSYMBOL_TOKEN_ELSE = 78,                /* TOKEN_ELSE  */
  YYSYMBOL_TOKEN_SWITCH = 79,              /* TOKEN_SWITCH  */
  YYSYMBOL_TOKEN_CASE = 80,                /* TOKEN_CASE  */
  YYSYMBOL_TOKEN_FOR = 81,                 /* TOKEN_FOR  */
  YYSYMBOL_TOKEN_WHILE = 82,               /* TOKEN_WHILE  */
  YYSYMBOL_TOKEN_DO = 83,                  /* TOKEN_DO  */
  YYSYMBOL_TOKEN_RETURN = 84,              /* TOKEN_RETURN  */
  YYSYMBOL_TOKEN_BREAK = 85,               /* TOKEN_BREAK  */
  YYSYMBOL_TOKEN_CONTINUE = 86,            /* TOKEN_CONTINUE  */
  YYSYMBOL_TOKEN_THIS = 87,                /* TOKEN_THIS  */
  YYSYMBOL_TOKEN_SUPER = 88,               /* TOKEN_SUPER  */
  YYSYMBOL_TOKEN_PRIVATE = 89,             /* TOKEN_PRIVATE  */
  YYSYMBOL_TOKEN_PROTECTED = 90,           /* TOKEN_PROTECTED  */
  YYSYMBOL_TOKEN_PUBLIC = 91,              /* TOKEN_PUBLIC  */
  YYSYMBOL_TOKEN_EVENT = 92,               /* TOKEN_EVENT  */
  YYSYMBOL_TOKEN_TESTONLY = 93,            /* TOKEN_TESTONLY  */
  YYSYMBOL_94_ = 94,                       /* '['  */
  YYSYMBOL_95_ = 95,                       /* '='  */
  YYSYMBOL_96_ = 96,                       /* ']'  */
  YYSYMBOL_97_ = 97,                       /* '{'  */
  YYSYMBOL_98_ = 98,                       /* '}'  */
  YYSYMBOL_99_ = 99,                       /* ':'  */
  YYSYMBOL_100_ = 100,                     /* ','  */
  YYSYMBOL_101_ = 101,                     /* '-'  */
  YYSYMBOL_102_ = 102,                     /* '.'  */
  YYSYMBOL_103_ = 103,                     /* '('  */
  YYSYMBOL_104_ = 104,                     /* ')'  */
  YYSYMBOL_105_ = 105,                     /* '|'  */
  YYSYMBOL_106_ = 106,                     /* '&'  */
  YYSYMBOL_107_ = 107,                     /* '^'  */
  YYSYMBOL_108_ = 108,                     /* '<'  */
  YYSYMBOL_109_ = 109,                     /* '>'  */
  YYSYMBOL_110_ = 110,                     /* '+'  */
  YYSYMBOL_111_ = 111,                     /* '*'  */
  YYSYMBOL_112_ = 112,                     /* '/'  */
  YYSYMBOL_113_ = 113,                     /* '%'  */
  YYSYMBOL_114_ = 114,                     /* '!'  */
  YYSYMBOL_115_ = 115,                     /* '~'  */
  YYSYMBOL_116_ = 116,                     /* ';'  */
  YYSYMBOL_YYACCEPT = 117,                 /* $accept  */
  YYSYMBOL_program_file = 118,             /* program_file  */
  YYSYMBOL_file_declaration_list = 119,    /* file_declaration_list  */
  YYSYMBOL_file_declaration = 120,         /* file_declaration  */
  YYSYMBOL_file_import_flag = 121,         /* file_import_flag  */
  YYSYMBOL_file_importonly_flag = 122,     /* file_importonly_flag  */
  YYSYMBOL_file_holy_flag = 123,           /* file_holy_flag  */
  YYSYMBOL_file_elem_declaration = 124,    /* file_elem_declaration  */
  YYSYMBOL_file_compound_type_declaration = 125, /* file_compound_type_declaration  */
  YYSYMBOL_class_flags = 126,              /* class_flags  */
  YYSYMBOL_struct_flags = 127,             /* struct_flags  */
  YYSYMBOL_function_flags = 128,           /* function_flags  */
  YYSYMBOL_var_flags = 129,                /* var_flags  */
  YYSYMBOL_import_flag = 130,              /* import_flag  */
  YYSYMBOL_importonly_flag = 131,          /* importonly_flag  */
  YYSYMBOL_holy_flag = 132,                /* holy_flag  */
  YYSYMBOL_access_flag = 133,              /* access_flag  */
  YYSYMBOL_attributes = 134,               /* attributes  */
  YYSYMBOL_class_dcl = 135,                /* class_dcl  */
  YYSYMBOL_class_header = 136,             /* class_header  */
  YYSYMBOL_class_elem_list = 137,          /* class_elem_list  */
  YYSYMBOL_class_element = 138,            /* class_element  */
  YYSYMBOL_class_access_and_import_flags = 139, /* class_access_and_import_flags  */
  YYSYMBOL_class_element_inner = 140,      /* class_element_inner  */
  YYSYMBOL_class_extends = 141,            /* class_extends  */
  YYSYMBOL_var_dcl = 142,                  /* var_dcl  */
  YYSYMBOL_struct_dcl = 143,               /* struct_dcl  */
  YYSYMBOL_struct_info = 144,              /* struct_info  */
  YYSYMBOL_struct_elem_list = 145,         /* struct_elem_list  */
  YYSYMBOL_struct_element = 146,           /* struct_element  */
  YYSYMBOL_struct_access_and_import_flags = 147, /* struct_access_and_import_flags  */
  YYSYMBOL_struct_element_inner = 148,     /* struct_element_inner  */
  YYSYMBOL_struct_extends = 149,           /* struct_extends  */
  YYSYMBOL_enum_dcl = 150,                 /* enum_dcl  */
  YYSYMBOL_enum_info = 151,                /* enum_info  */
  YYSYMBOL_enum_name_list = 152,           /* enum_name_list  */
  YYSYMBOL_enum_name = 153,                /* enum_name  */
  YYSYMBOL_browsable_dcl = 154,            /* browsable_dcl  */
  YYSYMBOL_browsable_list = 155,           /* browsable_list  */
  YYSYMBOL_browsable_val = 156,            /* browsable_val  */
  YYSYMBOL_hints_dcl = 157,                /* hints_dcl  */
  YYSYMBOL_hints_list = 158,               /* hints_list  */
  YYSYMBOL_hint_val = 159,                 /* hint_val  */
  YYSYMBOL_defaults_dcl = 160,             /* defaults_dcl  */
  YYSYMBOL_defaults_val_list = 161,        /* defaults_val_list  */
  YYSYMBOL_default_val = 162,              /* default_val  */
  YYSYMBOL_default_val_const = 163,        /* default_val_const  */
  YYSYMBOL_default_arithmetic_val_const = 164, /* default_arithmetic_val_const  */
  YYSYMBOL_function_dcl = 165,             /* function_dcl  */
  YYSYMBOL_function_header = 166,          /* function_header  */
  YYSYMBOL_function_ret_value = 167,       /* function_ret_value  */
  YYSYMBOL_function_ret_value_flag = 168,  /* function_ret_value_flag  */
  YYSYMBOL_function_inner_header = 169,    /* function_inner_header  */
  YYSYMBOL_function_name = 170,            /* function_name  */
  YYSYMBOL_function_op_name = 171,         /* function_op_name  */
  YYSYMBOL_function_params = 172,          /* function_params  */
  YYSYMBOL_function_param_list = 173,      /* function_param_list  */
  YYSYMBOL_function_param = 174,           /* function_param  */
  YYSYMBOL_function_param_ident_list = 175, /* function_param_ident_list  */
  YYSYMBOL_function_param_flag_list = 176, /* function_param_flag_list  */
  YYSYMBOL_function_param_flag = 177,      /* function_param_flag  */
  YYSYMBOL_function_tail = 178,            /* function_tail  */
  YYSYMBOL_function_body = 179,            /* function_body  */
  YYSYMBOL_function_local_vars_dcl = 180,  /* function_local_vars_dcl  */
  YYSYMBOL_function_local_vars_header = 181, /* function_local_vars_header  */
  YYSYMBOL_function_local_var_flag = 182,  /* function_local_var_flag  */
  YYSYMBOL_function_local_var_dcl = 183,   /* function_local_var_dcl  */
  YYSYMBOL_function_local_var_single_dcl = 184, /* function_local_var_single_dcl  */
  YYSYMBOL_function_local_var_initializer = 185, /* function_local_var_initializer  */
  YYSYMBOL_obj_type = 186,                 /* obj_type  */
  YYSYMBOL_full_type = 187,                /* full_type  */
  YYSYMBOL_ref_type = 188,                 /* ref_type  */
  YYSYMBOL_array_type = 189,               /* array_type  */
  YYSYMBOL_static_array_type = 190,        /* static_array_type  */
  YYSYMBOL_integer = 191,                  /* integer  */
  YYSYMBOL_string = 192,                   /* string  */
  YYSYMBOL_weak_type = 193,                /* weak_type  */
  YYSYMBOL_ident = 194,                    /* ident  */
  YYSYMBOL_semicolon = 195,                /* semicolon  */
  YYSYMBOL_optional_semicolon = 196,       /* optional_semicolon  */
  YYSYMBOL_optional_comma = 197            /* optional_comma  */
};
typedef enum yysymbol_kind_t yysymbol_kind_t;




#ifdef short
# undef short
#endif

/* On compilers that do not define __PTRDIFF_MAX__ etc., make sure
   <limits.h> and (if available) <stdint.h> are included
   so that the code can choose integer types of a good width.  */

#ifndef __PTRDIFF_MAX__
# include <limits.h> /* INFRINGES ON USER NAME SPACE */
# if defined __STDC_VERSION__ && 199901 <= __STDC_VERSION__
#  include <stdint.h> /* INFRINGES ON USER NAME SPACE */
#  define YY_STDINT_H
# endif
#endif

/* Narrow types that promote to a signed type and that can represent a
   signed or unsigned integer of at least N bits.  In tables they can
   save space and decrease cache pressure.  Promoting to a signed type
   helps avoid bugs in integer arithmetic.  */

#ifdef __INT_LEAST8_MAX__
typedef __INT_LEAST8_TYPE__ yytype_int8;
#elif defined YY_STDINT_H
typedef int_least8_t yytype_int8;
#else
typedef signed char yytype_int8;
#endif

#ifdef __INT_LEAST16_MAX__
typedef __INT_LEAST16_TYPE__ yytype_int16;
#elif defined YY_STDINT_H
typedef int_least16_t yytype_int16;
#else
typedef short yytype_int16;
#endif

/* Work around bug in HP-UX 11.23, which defines these macros
   incorrectly for preprocessor constants.  This workaround can likely
   be removed in 2023, as HPE has promised support for HP-UX 11.23
   (aka HP-UX 11i v2) only through the end of 2022; see Table 2 of
   <https://h20195.www2.hpe.com/V2/getpdf.aspx/4AA4-7673ENW.pdf>.  */
#ifdef __hpux
# undef UINT_LEAST8_MAX
# undef UINT_LEAST16_MAX
# define UINT_LEAST8_MAX 255
# define UINT_LEAST16_MAX 65535
#endif

#if defined __UINT_LEAST8_MAX__ && __UINT_LEAST8_MAX__ <= __INT_MAX__
typedef __UINT_LEAST8_TYPE__ yytype_uint8;
#elif (!defined __UINT_LEAST8_MAX__ && defined YY_STDINT_H \
       && UINT_LEAST8_MAX <= INT_MAX)
typedef uint_least8_t yytype_uint8;
#elif !defined __UINT_LEAST8_MAX__ && UCHAR_MAX <= INT_MAX
typedef unsigned char yytype_uint8;
#else
typedef short yytype_uint8;
#endif

#if defined __UINT_LEAST16_MAX__ && __UINT_LEAST16_MAX__ <= __INT_MAX__
typedef __UINT_LEAST16_TYPE__ yytype_uint16;
#elif (!defined __UINT_LEAST16_MAX__ && defined YY_STDINT_H \
       && UINT_LEAST16_MAX <= INT_MAX)
typedef uint_least16_t yytype_uint16;
#elif !defined __UINT_LEAST16_MAX__ && USHRT_MAX <= INT_MAX
typedef unsigned short yytype_uint16;
#else
typedef int yytype_uint16;
#endif

#ifndef YYPTRDIFF_T
# if defined __PTRDIFF_TYPE__ && defined __PTRDIFF_MAX__
#  define YYPTRDIFF_T __PTRDIFF_TYPE__
#  define YYPTRDIFF_MAXIMUM __PTRDIFF_MAX__
# elif defined PTRDIFF_MAX
#  ifndef ptrdiff_t
#   include <stddef.h> /* INFRINGES ON USER NAME SPACE */
#  endif
#  define YYPTRDIFF_T ptrdiff_t
#  define YYPTRDIFF_MAXIMUM PTRDIFF_MAX
# else
#  define YYPTRDIFF_T long
#  define YYPTRDIFF_MAXIMUM LONG_MAX
# endif
#endif

#ifndef YYSIZE_T
# ifdef __SIZE_TYPE__
#  define YYSIZE_T __SIZE_TYPE__
# elif defined size_t
#  define YYSIZE_T size_t
# elif defined __STDC_VERSION__ && 199901 <= __STDC_VERSION__
#  include <stddef.h> /* INFRINGES ON USER NAME SPACE */
#  define YYSIZE_T size_t
# else
#  define YYSIZE_T unsigned
# endif
#endif

#define YYSIZE_MAXIMUM                                  \
  YY_CAST (YYPTRDIFF_T,                                 \
           (YYPTRDIFF_MAXIMUM < YY_CAST (YYSIZE_T, -1)  \
            ? YYPTRDIFF_MAXIMUM                         \
            : YY_CAST (YYSIZE_T, -1)))

#define YYSIZEOF(X) YY_CAST (YYPTRDIFF_T, sizeof (X))


/* Stored state numbers (used for stacks). */
typedef yytype_int16 yy_state_t;

/* State numbers in computations.  */
typedef int yy_state_fast_t;

#ifndef YY_
# if defined YYENABLE_NLS && YYENABLE_NLS
#  if ENABLE_NLS
#   include <libintl.h> /* INFRINGES ON USER NAME SPACE */
#   define YY_(Msgid) dgettext ("bison-runtime", Msgid)
#  endif
# endif
# ifndef YY_
#  define YY_(Msgid) Msgid
# endif
#endif


#ifndef YY_ATTRIBUTE_PURE
# if defined __GNUC__ && 2 < __GNUC__ + (96 <= __GNUC_MINOR__)
#  define YY_ATTRIBUTE_PURE __attribute__ ((__pure__))
# else
#  define YY_ATTRIBUTE_PURE
# endif
#endif

#ifndef YY_ATTRIBUTE_UNUSED
# if defined __GNUC__ && 2 < __GNUC__ + (7 <= __GNUC_MINOR__)
#  define YY_ATTRIBUTE_UNUSED __attribute__ ((__unused__))
# else
#  define YY_ATTRIBUTE_UNUSED
# endif
#endif

/* Suppress unused-variable warnings by "using" E.  */
#if ! defined lint || defined __GNUC__
# define YY_USE(E) ((void) (E))
#else
# define YY_USE(E) /* empty */
#endif

/* Suppress an incorrect diagnostic about yylval being uninitialized.  */
#if defined __GNUC__ && ! defined __ICC && 406 <= __GNUC__ * 100 + __GNUC_MINOR__
# if __GNUC__ * 100 + __GNUC_MINOR__ < 407
#  define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN                           \
    _Pragma ("GCC diagnostic push")                                     \
    _Pragma ("GCC diagnostic ignored \"-Wuninitialized\"")
# else
#  define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN                           \
    _Pragma ("GCC diagnostic push")                                     \
    _Pragma ("GCC diagnostic ignored \"-Wuninitialized\"")              \
    _Pragma ("GCC diagnostic ignored \"-Wmaybe-uninitialized\"")
# endif
# define YY_IGNORE_MAYBE_UNINITIALIZED_END      \
    _Pragma ("GCC diagnostic pop")
#else
# define YY_INITIAL_VALUE(Value) Value
#endif
#ifndef YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
# define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
# define YY_IGNORE_MAYBE_UNINITIALIZED_END
#endif
#ifndef YY_INITIAL_VALUE
# define YY_INITIAL_VALUE(Value) /* Nothing. */
#endif

#if defined __cplusplus && defined __GNUC__ && ! defined __ICC && 6 <= __GNUC__
# define YY_IGNORE_USELESS_CAST_BEGIN                          \
    _Pragma ("GCC diagnostic push")                            \
    _Pragma ("GCC diagnostic ignored \"-Wuseless-cast\"")
# define YY_IGNORE_USELESS_CAST_END            \
    _Pragma ("GCC diagnostic pop")
#endif
#ifndef YY_IGNORE_USELESS_CAST_BEGIN
# define YY_IGNORE_USELESS_CAST_BEGIN
# define YY_IGNORE_USELESS_CAST_END
#endif


#define YY_ASSERT(E) ((void) (0 && (E)))

#if !defined yyoverflow

/* The parser invokes alloca or malloc; define the necessary symbols.  */

# ifdef YYSTACK_USE_ALLOCA
#  if YYSTACK_USE_ALLOCA
#   ifdef __GNUC__
#    define YYSTACK_ALLOC __builtin_alloca
#   elif defined __BUILTIN_VA_ARG_INCR
#    include <alloca.h> /* INFRINGES ON USER NAME SPACE */
#   elif defined _AIX
#    define YYSTACK_ALLOC __alloca
#   elif defined _MSC_VER
#    include <malloc.h> /* INFRINGES ON USER NAME SPACE */
#    define alloca _alloca
#   else
#    define YYSTACK_ALLOC alloca
#    if ! defined _ALLOCA_H && ! defined EXIT_SUCCESS
#     include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
      /* Use EXIT_SUCCESS as a witness for stdlib.h.  */
#     ifndef EXIT_SUCCESS
#      define EXIT_SUCCESS 0
#     endif
#    endif
#   endif
#  endif
# endif

# ifdef YYSTACK_ALLOC
   /* Pacify GCC's 'empty if-body' warning.  */
#  define YYSTACK_FREE(Ptr) do { /* empty */; } while (0)
#  ifndef YYSTACK_ALLOC_MAXIMUM
    /* The OS might guarantee only one guard page at the bottom of the stack,
       and a page size can be as small as 4096 bytes.  So we cannot safely
       invoke alloca (N) if N exceeds 4096.  Use a slightly smaller number
       to allow for a few compiler-allocated temporary stack slots.  */
#   define YYSTACK_ALLOC_MAXIMUM 4032 /* reasonable circa 2006 */
#  endif
# else
#  define YYSTACK_ALLOC YYMALLOC
#  define YYSTACK_FREE YYFREE
#  ifndef YYSTACK_ALLOC_MAXIMUM
#   define YYSTACK_ALLOC_MAXIMUM YYSIZE_MAXIMUM
#  endif
#  if (defined __cplusplus && ! defined EXIT_SUCCESS \
       && ! ((defined YYMALLOC || defined malloc) \
             && (defined YYFREE || defined free)))
#   include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
#   ifndef EXIT_SUCCESS
#    define EXIT_SUCCESS 0
#   endif
#  endif
#  ifndef YYMALLOC
#   define YYMALLOC malloc
#   if ! defined malloc && ! defined EXIT_SUCCESS
void *malloc (YYSIZE_T); /* INFRINGES ON USER NAME SPACE */
#   endif
#  endif
#  ifndef YYFREE
#   define YYFREE free
#   if ! defined free && ! defined EXIT_SUCCESS
void free (void *); /* INFRINGES ON USER NAME SPACE */
#   endif
#  endif
# endif
#endif /* !defined yyoverflow */

#if (! defined yyoverflow \
     && (! defined __cplusplus \
         || (defined YYSTYPE_IS_TRIVIAL && YYSTYPE_IS_TRIVIAL)))

/* A type that is properly aligned for any stack member.  */
union yyalloc
{
  yy_state_t yyss_alloc;
  YYSTYPE yyvs_alloc;
};

/* The size of the maximum gap between one aligned stack and the next.  */
# define YYSTACK_GAP_MAXIMUM (YYSIZEOF (union yyalloc) - 1)

/* The size of an array large to enough to hold all stacks, each with
   N elements.  */
# define YYSTACK_BYTES(N) \
     ((N) * (YYSIZEOF (yy_state_t) + YYSIZEOF (YYSTYPE)) \
      + YYSTACK_GAP_MAXIMUM)

# define YYCOPY_NEEDED 1

/* Relocate STACK from its old location to the new one.  The
   local variables YYSIZE and YYSTACKSIZE give the old and new number of
   elements in the stack, and YYPTR gives the new location of the
   stack.  Advance YYPTR to a properly aligned location for the next
   stack.  */
# define YYSTACK_RELOCATE(Stack_alloc, Stack)                           \
    do                                                                  \
      {                                                                 \
        YYPTRDIFF_T yynewbytes;                                         \
        YYCOPY (&yyptr->Stack_alloc, Stack, yysize);                    \
        Stack = &yyptr->Stack_alloc;                                    \
        yynewbytes = yystacksize * YYSIZEOF (*Stack) + YYSTACK_GAP_MAXIMUM; \
        yyptr += yynewbytes / YYSIZEOF (*yyptr);                        \
      }                                                                 \
    while (0)

#endif

#if defined YYCOPY_NEEDED && YYCOPY_NEEDED
/* Copy COUNT objects from SRC to DST.  The source and destination do
   not overlap.  */
# ifndef YYCOPY
#  if defined __GNUC__ && 1 < __GNUC__
#   define YYCOPY(Dst, Src, Count) \
      __builtin_memcpy (Dst, Src, YY_CAST (YYSIZE_T, (Count)) * sizeof (*(Src)))
#  else
#   define YYCOPY(Dst, Src, Count)              \
      do                                        \
        {                                       \
          YYPTRDIFF_T yyi;                      \
          for (yyi = 0; yyi < (Count); yyi++)   \
            (Dst)[yyi] = (Src)[yyi];            \
        }                                       \
      while (0)
#  endif
# endif
#endif /* !YYCOPY_NEEDED */

/* YYFINAL -- State number of the termination state.  */
#define YYFINAL  3
/* YYLAST -- Last index in YYTABLE.  */
#define YYLAST   469

/* YYNTOKENS -- Number of terminals.  */
#define YYNTOKENS  117
/* YYNNTS -- Number of nonterminals.  */
#define YYNNTS  81
/* YYNRULES -- Number of rules.  */
#define YYNRULES  210
/* YYNSTATES -- Number of states.  */
#define YYNSTATES  343

/* YYMAXUTOK -- Last valid token kind.  */
#define YYMAXUTOK   348


/* YYTRANSLATE(TOKEN-NUM) -- Symbol number corresponding to TOKEN-NUM
   as returned by yylex, with out-of-bounds checking.  */
#define YYTRANSLATE(YYX)                                \
  (0 <= (YYX) && (YYX) <= YYMAXUTOK                     \
   ? YY_CAST (yysymbol_kind_t, yytranslate[YYX])        \
   : YYSYMBOL_YYUNDEF)

/* YYTRANSLATE[TOKEN-NUM] -- Symbol number corresponding to TOKEN-NUM
   as returned by yylex.  */
static const yytype_int8 yytranslate[] =
{
       0,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,   114,     2,     2,     2,   113,   106,     2,
     103,   104,   111,   110,   100,   101,   102,   112,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,    99,   116,
     108,    95,   109,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,    94,     2,    96,   107,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,    97,   105,    98,   115,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     1,     2,     3,     4,
       5,     6,     7,     8,     9,    10,    11,    12,    13,    14,
      15,    16,    17,    18,    19,    20,    21,    22,    23,    24,
      25,    26,    27,    28,    29,    30,    31,    32,    33,    34,
      35,    36,    37,    38,    39,    40,    41,    42,    43,    44,
      45,    46,    47,    48,    49,    50,    51,    52,    53,    54,
      55,    56,    57,    58,    59,    60,    61,    62,    63,    64,
      65,    66,    67,    68,    69,    70,    71,    72,    73,    74,
      75,    76,    77,    78,    79,    80,    81,    82,    83,    84,
      85,    86,    87,    88,    89,    90,    91,    92,    93
};

#if YYDEBUG
/* YYRLINE[YYN] -- Source line where rule number YYN was defined.  */
static const yytype_int16 yyrline[] =
{
       0,   267,   267,   275,   276,   280,   281,   282,   286,   290,
     294,   298,   299,   300,   301,   302,   306,   307,   315,   316,
     317,   318,   319,   320,   321,   325,   326,   327,   331,   332,
     333,   334,   335,   336,   337,   338,   339,   340,   341,   342,
     343,   344,   348,   349,   350,   351,   352,   353,   354,   355,
     356,   364,   365,   369,   373,   377,   378,   379,   380,   384,
     388,   392,   396,   407,   411,   415,   416,   420,   421,   422,
     423,   427,   431,   432,   433,   434,   438,   439,   447,   470,
     474,   478,   479,   483,   484,   485,   486,   490,   494,   495,
     496,   497,   501,   502,   511,   515,   516,   524,   525,   529,
     530,   531,   538,   539,   543,   544,   548,   549,   557,   558,
     562,   563,   567,   575,   576,   580,   581,   585,   586,   587,
     596,   597,   598,   599,   600,   601,   602,   603,   607,   608,
     616,   620,   624,   625,   629,   630,   634,   640,   649,   650,
     651,   652,   656,   657,   658,   659,   660,   661,   662,   663,
     664,   665,   666,   667,   668,   669,   670,   671,   672,   673,
     674,   675,   676,   677,   678,   679,   680,   684,   685,   689,
     690,   694,   706,   707,   711,   712,   716,   717,   718,   719,
     723,   724,   728,   732,   733,   734,   738,   742,   743,   747,
     748,   752,   763,   771,   772,   776,   777,   778,   779,   783,
     787,   791,   795,   809,   813,   817,   821,   825,   826,   830,
     831
};
#endif

/** Accessing symbol of state STATE.  */
#define YY_ACCESSING_SYMBOL(State) YY_CAST (yysymbol_kind_t, yystos[State])

#if YYDEBUG || 0
/* The user-facing name of the symbol whose (internal) number is
   YYSYMBOL.  No bounds checking.  */
static const char *yysymbol_name (yysymbol_kind_t yysymbol) YY_ATTRIBUTE_UNUSED;

/* YYTNAME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
   First, the terminals, then, starting at YYNTOKENS, nonterminals.  */
static const char *const yytname[] =
{
  "\"end of file\"", "error", "\"invalid token\"", "TOKEN_IDENT",
  "TOKEN_INTEGER", "TOKEN_FLOAT", "TOKEN_NAME", "TOKEN_STRING",
  "TOKEN_TDBID", "TOKEN_RESREF", "TOKEN_BOOL_TRUE", "TOKEN_BOOL_FALSE",
  "TOKEN_NULL", "TOKEN_IMPORT", "TOKEN_IMPORTONLY", "TOKEN_HOLY",
  "TOKEN_ABSTRACT", "TOKEN_CONST", "TOKEN_MUTABLE", "TOKEN_EXTENDS",
  "TOKEN_CLASS", "TOKEN_ENUM", "TOKEN_STRUCT", "TOKEN_FUNCTION",
  "TOKEN_DEF", "TOKEN_EDITABLE", "TOKEN_INSTANCE_EDITABLE",
  "TOKEN_REPLICATED", "TOKEN_PERSISTENT", "TOKEN_FINAL", "TOKEN_VIRTUAL",
  "TOKEN_OVERRIDE", "TOKEN_OUT", "TOKEN_OPTIONAL", "TOKEN_SKIP",
  "TOKEN_INLINED", "TOKEN_ARRAY", "TOKEN_HINT", "TOKEN_HINTS",
  "TOKEN_DEFAULT", "TOKEN_DEFAULTS", "TOKEN_BROWSABLE", "TOKEN_VAR",
  "TOKEN_EXEC", "TOKEN_TIMER", "TOKEN_WEAK", "TOKEN_OPERATOR",
  "TOKEN_CAST", "TOKEN_IMPLICIT", "TOKEN_STATIC", "TOKEN_VISUAL",
  "TOKEN_CORE", "TOKEN_LOGIC", "TOKEN_MULTICAST", "TOKEN_HOST",
  "TOKEN_CLIENT", "TOKEN_RELIABLE", "TOKEN_QUEST", "TOKEN_REF",
  "TOKEN_DEBUG", "TOKEN_SYNCHRONIZED", "TOKEN_SIMPLE_TYPE",
  "TOKEN_CLASS_TYPE", "TOKEN_ENUM_TYPE", "TOKEN_OP_IADD", "TOKEN_OP_ISUB",
  "TOKEN_OP_IMUL", "TOKEN_OP_IDIV", "TOKEN_OP_IAND", "TOKEN_OP_IOR",
  "TOKEN_OP_LOGIC_OR", "TOKEN_OP_LOGIC_AND", "TOKEN_OP_EQUAL",
  "TOKEN_OP_NOTEQUAL", "TOKEN_OP_GREQ", "TOKEN_OP_LEEQ", "TOKEN_NEW",
  "TOKEN_IF", "TOKEN_ELSE", "TOKEN_SWITCH", "TOKEN_CASE", "TOKEN_FOR",
  "TOKEN_WHILE", "TOKEN_DO", "TOKEN_RETURN", "TOKEN_BREAK",
  "TOKEN_CONTINUE", "TOKEN_THIS", "TOKEN_SUPER", "TOKEN_PRIVATE",
  "TOKEN_PROTECTED", "TOKEN_PUBLIC", "TOKEN_EVENT", "TOKEN_TESTONLY",
  "'['", "'='", "']'", "'{'", "'}'", "':'", "','", "'-'", "'.'", "'('",
  "')'", "'|'", "'&'", "'^'", "'<'", "'>'", "'+'", "'*'", "'/'", "'%'",
  "'!'", "'~'", "';'", "$accept", "program_file", "file_declaration_list",
  "file_declaration", "file_import_flag", "file_importonly_flag",
  "file_holy_flag", "file_elem_declaration",
  "file_compound_type_declaration", "class_flags", "struct_flags",
  "function_flags", "var_flags", "import_flag", "importonly_flag",
  "holy_flag", "access_flag", "attributes", "class_dcl", "class_header",
  "class_elem_list", "class_element", "class_access_and_import_flags",
  "class_element_inner", "class_extends", "var_dcl", "struct_dcl",
  "struct_info", "struct_elem_list", "struct_element",
  "struct_access_and_import_flags", "struct_element_inner",
  "struct_extends", "enum_dcl", "enum_info", "enum_name_list", "enum_name",
  "browsable_dcl", "browsable_list", "browsable_val", "hints_dcl",
  "hints_list", "hint_val", "defaults_dcl", "defaults_val_list",
  "default_val", "default_val_const", "default_arithmetic_val_const",
  "function_dcl", "function_header", "function_ret_value",
  "function_ret_value_flag", "function_inner_header", "function_name",
  "function_op_name", "function_params", "function_param_list",
  "function_param", "function_param_ident_list",
  "function_param_flag_list", "function_param_flag", "function_tail",
  "function_body", "function_local_vars_dcl", "function_local_vars_header",
  "function_local_var_flag", "function_local_var_dcl",
  "function_local_var_single_dcl", "function_local_var_initializer",
  "obj_type", "full_type", "ref_type", "array_type", "static_array_type",
  "integer", "string", "weak_type", "ident", "semicolon",
  "optional_semicolon", "optional_comma", YY_NULLPTR
};

static const char *
yysymbol_name (yysymbol_kind_t yysymbol)
{
  return yytname[yysymbol];
}
#endif

#define YYPACT_NINF (-284)

#define yypact_value_is_default(Yyn) \
  ((Yyn) == YYPACT_NINF)

#define YYTABLE_NINF (-189)

#define yytable_value_is_error(Yyn) \
  0

/* YYPACT[STATE-NUM] -- Index in YYTABLE of the portion describing
   STATE-NUM.  */
static const yytype_int16 yypact[] =
{
    -284,    48,   168,  -284,  -284,  -284,  -284,  -284,   177,    31,
      31,  -284,  -284,  -284,     8,   122,    47,   231,   122,   122,
     122,   122,   122,   122,   122,   122,   122,   122,     8,     8,
       8,    47,   231,    47,   -61,    56,   103,    15,    37,  -284,
      34,  -284,    52,  -284,    57,  -284,  -284,    53,    31,    31,
     -61,  -284,  -284,   -61,     8,     8,  -284,   122,   122,  -284,
    -284,    41,  -284,  -284,  -284,  -284,  -284,  -284,  -284,  -284,
    -284,  -284,  -284,  -284,  -284,  -284,  -284,  -284,  -284,  -284,
    -284,  -284,     0,  -284,  -284,    47,    47,    47,   304,  -284,
     110,  -284,  -284,  -284,  -284,    47,   211,  -284,  -284,    47,
     376,    67,   143,   145,  -284,  -284,  -284,  -284,  -284,  -284,
    -284,  -284,  -284,  -284,  -284,  -284,  -284,    73,  -284,  -284,
    -284,  -284,  -284,  -284,  -284,  -284,  -284,  -284,  -284,  -284,
    -284,  -284,    69,   217,    70,  -284,    79,  -284,  -284,  -284,
    -284,    76,  -284,    84,    47,   206,  -284,  -284,  -284,  -284,
    -284,  -284,  -284,  -284,  -284,    91,  -284,    47,  -284,    47,
    -284,  -284,    47,    98,    47,   107,    19,  -284,  -284,  -284,
    -284,   187,   101,  -284,   274,  -284,  -284,  -284,  -284,  -284,
     187,   101,  -284,   274,  -284,  -284,  -284,  -284,    47,   115,
      12,   106,   206,   117,   124,  -284,    67,  -284,  -284,  -284,
     134,    47,  -284,   141,    47,    47,  -284,   142,  -284,   274,
     308,   184,   184,   184,   184,   184,   184,   308,   175,   119,
    -284,  -284,   274,   119,  -284,  -284,  -284,  -284,   238,  -284,
     229,   -57,  -284,    97,    47,  -284,   242,   152,    47,    25,
     154,    47,   161,    47,   126,   119,  -284,  -284,   184,   184,
    -284,  -284,  -284,  -284,  -284,  -284,  -284,    47,  -284,  -284,
     119,  -284,  -284,  -284,  -284,    97,     4,  -284,  -284,   155,
     156,   157,  -284,   178,  -284,  -284,  -284,  -284,  -284,  -284,
    -284,   119,  -284,  -284,   171,   140,   119,  -284,  -284,  -284,
    -284,   119,   119,  -284,   179,  -284,  -284,  -284,  -284,    47,
     234,    39,    39,    39,   238,  -284,   276,  -284,  -284,   119,
    -284,  -284,  -284,    39,   183,   190,   166,    17,  -284,   -45,
     -37,   -20,   194,   119,  -284,   178,  -284,    97,     4,  -284,
      47,  -284,  -284,  -284,  -284,  -284,  -284,  -284,   180,  -284,
     193,     4,  -284
};

/* YYDEFACT[STATE-NUM] -- Default reduction number in state STATE-NUM.
   Performed when YYTABLE does not specify something else to do.  Zero
   means the default is an error.  */
static const yytype_uint8 yydefact[] =
{
       4,     0,    52,     1,    51,    53,    54,     3,    41,    24,
      24,     8,     9,    10,    24,    41,     0,    41,    41,    41,
      41,    41,    41,    41,    41,    41,    41,    41,    24,    24,
      24,     0,    41,     0,   208,     0,     0,     0,    41,    13,
       0,    11,     0,    12,     0,    15,   130,     0,    24,    24,
     208,    17,    16,   208,    24,    24,    18,    41,    41,    37,
     205,    95,    22,    25,    39,    35,    36,    28,    29,    30,
      31,    32,    33,    34,    38,    19,    20,    21,   137,    23,
      26,    40,     0,   207,     5,     0,     0,     0,     0,   140,
       0,   136,    14,    66,    82,     0,   168,     6,     7,     0,
       0,    62,    77,    93,   138,   142,   143,   144,   145,   146,
     147,   148,   149,   153,   154,   157,   158,     0,   165,   150,
     151,   152,   155,   156,   159,   160,   161,   162,   163,   164,
     139,   141,    58,    58,   210,    98,    99,   179,   176,   178,
     177,     0,   167,   170,     0,   175,    96,   120,   121,   123,
     122,   124,   125,   126,   127,     0,    61,     0,    64,     0,
      80,   166,     0,     0,     0,     0,     0,    55,    56,    57,
      63,    52,    58,    65,    41,    67,    68,    69,    70,    79,
      52,    58,    81,    41,    83,    84,    85,    86,   209,     0,
       0,   133,   175,     0,   173,   174,    60,    76,    92,   109,
       0,   111,   114,     0,   116,   105,   103,     0,    71,    41,
      41,    50,    50,    50,    50,    50,    50,    41,     0,     0,
      73,    87,    41,     0,    89,    97,    94,   202,     0,   100,
     135,     0,   169,     0,     0,    59,     0,     0,   111,     0,
       0,   116,     0,   105,     0,     0,    72,    42,    50,    50,
      43,    44,    45,    46,    48,    47,    49,     0,   206,    75,
       0,    88,    91,   101,   134,     0,   185,   131,   181,     0,
       0,     0,   171,   194,   193,   195,   196,   197,   198,   172,
     203,     0,   108,   110,     0,     0,     0,   113,   115,   102,
     104,     0,     0,    74,     0,    90,   132,   187,   182,     0,
       0,     0,     0,     0,     0,   112,     0,   128,   129,     0,
     117,   106,   107,     0,     0,     0,     0,   190,   186,     0,
       0,     0,     0,     0,   118,    78,   180,     0,   185,   192,
       0,   200,   204,   199,   201,   119,   191,   183,     0,   189,
     190,   185,   184
};

/* YYPGOTO[NTERM-NUM].  */
static const yytype_int16 yypgoto[] =
{
    -284,  -284,  -284,  -284,  -284,  -284,  -284,  -284,   284,   374,
      86,   -14,   220,   -48,  -284,  -284,  -108,   -81,   287,  -284,
    -284,  -284,   125,  -284,  -284,  -144,   290,  -284,  -284,  -284,
     129,  -284,  -284,  -284,  -284,  -284,   131,   199,    99,   146,
     207,    75,   185,   208,   108,   189,   109,  -284,   -36,  -284,
    -284,  -284,  -284,  -284,  -284,  -284,   162,  -284,  -130,   210,
    -284,  -284,  -284,  -283,  -284,  -284,    28,    29,  -284,  -242,
      43,  -284,  -284,  -284,  -202,  -284,  -284,   -16,  -204,    61,
    -284
};

/* YYDEFGOTO[NTERM-NUM].  */
static const yytype_int16 yydefgoto[] =
{
       0,     1,     2,     7,     8,     9,    10,    34,    50,    35,
      36,    37,   218,    11,    12,    13,   171,    38,    51,    40,
     132,   173,   174,   175,   158,   219,    52,    42,   133,   182,
     183,   184,   160,    43,    44,   134,   135,   176,   242,   243,
     177,   237,   238,   178,   240,   241,   155,   309,    45,    46,
     231,   265,    47,    91,   130,   141,   142,   143,   315,   144,
     145,   267,   314,   298,   299,   300,   316,   317,   338,   272,
     273,   274,   275,   276,   229,   281,   277,   278,   259,    84,
     189
};

/* YYTABLE[YYPACT[STATE-NUM]] -- What to do in state STATE-NUM.  If
   positive, shift that token.  If negative, reduce the rule whose
   number is the opposite.  If YYTABLE_NINF, syntax error.  */
static const yytype_int16 yytable[] =
{
      61,    59,    92,    64,    65,    66,    67,    68,    69,    70,
      71,    72,    73,    74,   193,    78,   227,    82,    81,   262,
     156,   297,    60,   296,    14,   180,   263,   268,   284,   147,
     148,   149,   150,   151,   152,   153,   154,    54,    87,   223,
     266,   293,    60,    64,    81,   337,  -188,    14,     3,   304,
      60,   172,   181,   -27,    15,    83,   295,   304,   342,   258,
      48,    88,    89,    90,   331,   245,    57,    18,    19,   102,
     103,   104,   332,   180,   304,   269,    85,   305,   260,   136,
      20,    21,   310,   146,   270,   336,    22,   311,   312,   333,
      23,    24,    25,    26,    27,   100,   101,    28,    29,    30,
      60,    55,   322,    63,   279,   324,   162,   163,   164,   165,
     166,    97,   329,   228,    98,   235,   205,   330,    80,   335,
      28,    29,    30,   208,    49,    86,   285,   294,   194,    31,
      58,    93,   221,   269,    63,    80,   291,   292,   220,    15,
      99,   197,   270,   198,   307,   308,   200,   224,   203,    94,
     207,    57,    18,    19,    95,   271,    96,   131,   167,   168,
     169,    33,   157,    33,   159,    20,    21,   170,    -2,   161,
     188,    22,   136,   246,   190,    23,    24,    25,    26,    27,
     191,     4,     5,     6,   192,   200,   261,   196,   203,   207,
     167,   168,   169,    14,    15,   201,    59,   -24,    16,   -27,
       4,   248,   211,    81,   204,   230,    17,    18,    19,   212,
     213,   214,   215,   226,  -175,    58,   233,   257,   194,   216,
      20,    21,   200,   137,   234,   203,    22,   207,   137,   236,
      23,    24,    25,    26,    27,   258,   239,   244,   138,   139,
     140,   194,   227,   138,   139,   140,   264,    14,    15,   280,
     282,   -24,   287,   -27,   162,   163,   164,   165,   166,   289,
      17,    18,    19,   301,   302,   303,    28,    29,    30,    31,
      32,    33,   304,   306,    20,    21,   318,   249,   313,   323,
      22,   326,   328,   194,    23,    24,    25,    26,    27,   327,
     334,   210,   211,   330,    53,    39,   341,   209,    41,   212,
     213,   214,   215,    57,    18,    19,   167,   168,   169,   216,
     222,    33,   206,   283,   194,   179,   -50,    20,    21,   225,
      28,    29,    30,    22,    32,   210,   211,    23,    24,    25,
      26,    27,   185,   212,   213,   214,   215,    57,    18,    19,
     186,   187,   290,   216,   319,   320,   321,   199,   286,   288,
     -50,    20,    21,   202,   232,   195,   325,    22,   339,   340,
       0,    23,    24,    25,    26,    27,    31,   217,   105,   106,
     107,   108,   109,   110,   111,   112,   113,   114,   115,   116,
     147,   148,   149,   150,   151,   152,   153,   154,    56,     0,
       0,    62,     0,     0,     0,     0,     0,     0,   117,     0,
       0,   217,    75,    76,    77,   118,    79,     0,     0,   119,
     120,   121,   122,   123,   124,   125,   126,   127,   128,   129,
       0,     0,    62,    79,     0,     0,     0,     0,    62,    79,
     247,   250,   251,   252,   253,   254,   255,   256,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,   247,   256
};

static const yytype_int16 yycheck[] =
{
      16,    15,    38,    17,    18,    19,    20,    21,    22,    23,
      24,    25,    26,    27,   144,    31,     4,    33,    32,   223,
     101,    17,     3,   265,    16,   133,   228,   231,     3,     4,
       5,     6,     7,     8,     9,    10,    11,    29,    23,   183,
      97,   245,     3,    57,    58,   328,    42,    16,     0,    94,
       3,   132,   133,    22,    17,   116,   260,    94,   341,   116,
      29,    46,    47,    48,   109,   209,    29,    30,    31,    85,
      86,    87,   109,   181,    94,    36,    20,   281,   222,    95,
      43,    44,   286,    99,    45,   327,    49,   291,   292,   109,
      53,    54,    55,    56,    57,    95,    96,    89,    90,    91,
       3,    93,   304,    17,   234,   309,    37,    38,    39,    40,
      41,    50,    95,   101,    53,   196,    97,   100,    32,   323,
      89,    90,    91,   171,    93,    22,   101,   257,   144,    92,
      93,    97,   180,    36,    48,    49,    10,    11,   174,    17,
      99,   157,    45,   159,     4,     5,   162,   183,   164,    97,
     166,    29,    30,    31,    97,    58,   103,    47,    89,    90,
      91,    94,    19,    94,    19,    43,    44,    98,     0,    96,
     100,    49,   188,   209,    95,    53,    54,    55,    56,    57,
     104,    13,    14,    15,   100,   201,   222,    96,   204,   205,
      89,    90,    91,    16,    17,    97,   210,    20,    21,    22,
      13,    17,    18,   217,    97,    99,    29,    30,    31,    25,
      26,    27,    28,    98,     3,    93,    99,    42,   234,    35,
      43,    44,   238,    17,   100,   241,    49,   243,    17,    95,
      53,    54,    55,    56,    57,   116,    95,    95,    32,    33,
      34,   257,     4,    32,    33,    34,    17,    16,    17,     7,
      98,    20,    98,    22,    37,    38,    39,    40,    41,    98,
      29,    30,    31,   108,   108,   108,    89,    90,    91,    92,
      93,    94,    94,   102,    43,    44,    42,    93,    99,     3,
      49,    98,   116,   299,    53,    54,    55,    56,    57,    99,
      96,    17,    18,   100,    10,     8,   116,   172,     8,    25,
      26,    27,    28,    29,    30,    31,    89,    90,    91,    35,
     181,    94,   166,   238,   330,    98,    42,    43,    44,   188,
      89,    90,    91,    49,    93,    17,    18,    53,    54,    55,
      56,    57,   133,    25,    26,    27,    28,    29,    30,    31,
     133,   133,   243,    35,   301,   302,   303,   162,   239,   241,
      42,    43,    44,   164,   192,   145,   313,    49,   330,   330,
      -1,    53,    54,    55,    56,    57,    92,    93,    64,    65,
      66,    67,    68,    69,    70,    71,    72,    73,    74,    75,
       4,     5,     6,     7,     8,     9,    10,    11,    14,    -1,
      -1,    17,    -1,    -1,    -1,    -1,    -1,    -1,    94,    -1,
      -1,    93,    28,    29,    30,   101,    32,    -1,    -1,   105,
     106,   107,   108,   109,   110,   111,   112,   113,   114,   115,
      -1,    -1,    48,    49,    -1,    -1,    -1,    -1,    54,    55,
     210,   211,   212,   213,   214,   215,   216,   217,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   248,   249
};

/* YYSTOS[STATE-NUM] -- The symbol kind of the accessing symbol of
   state STATE-NUM.  */
static const yytype_uint8 yystos[] =
{
       0,   118,   119,     0,    13,    14,    15,   120,   121,   122,
     123,   130,   131,   132,    16,    17,    21,    29,    30,    31,
      43,    44,    49,    53,    54,    55,    56,    57,    89,    90,
      91,    92,    93,    94,   124,   126,   127,   128,   134,   135,
     136,   143,   144,   150,   151,   165,   166,   169,    29,    93,
     125,   135,   143,   125,    29,    93,   126,    29,    93,   128,
       3,   194,   126,   127,   128,   128,   128,   128,   128,   128,
     128,   128,   128,   128,   128,   126,   126,   126,   194,   126,
     127,   128,   194,   116,   196,    20,    22,    23,    46,    47,
      48,   170,   165,    97,    97,    97,   103,   196,   196,    99,
      95,    96,   194,   194,   194,    64,    65,    66,    67,    68,
      69,    70,    71,    72,    73,    74,    75,    94,   101,   105,
     106,   107,   108,   109,   110,   111,   112,   113,   114,   115,
     171,    47,   137,   145,   152,   153,   194,    17,    32,    33,
      34,   172,   173,   174,   176,   177,   194,     4,     5,     6,
       7,     8,     9,    10,    11,   163,   134,    19,   141,    19,
     149,    96,    37,    38,    39,    40,    41,    89,    90,    91,
      98,   133,   134,   138,   139,   140,   154,   157,   160,    98,
     133,   134,   146,   147,   148,   154,   157,   160,   100,   197,
      95,   104,   100,   175,   194,   176,    96,   194,   194,   159,
     194,    97,   162,   194,    97,    97,   156,   194,   130,   139,
      17,    18,    25,    26,    27,    28,    35,    93,   129,   142,
     165,   130,   147,   142,   165,   153,    98,     4,   101,   191,
      99,   167,   173,    99,   100,   134,    95,   158,   159,    95,
     161,   162,   155,   156,    95,   142,   165,   129,    17,    93,
     129,   129,   129,   129,   129,   129,   129,    42,   116,   195,
     142,   165,   195,   191,    17,   168,    97,   178,   195,    36,
      45,    58,   186,   187,   188,   189,   190,   193,   194,   175,
       7,   192,    98,   158,     3,   101,   163,    98,   161,    98,
     155,    10,    11,   195,   175,   195,   186,    17,   180,   181,
     182,   108,   108,   108,    94,   195,   102,     4,     5,   164,
     195,   195,   195,    99,   179,   175,   183,   184,    42,   187,
     187,   187,   191,     3,   195,   187,    98,    99,   116,    95,
     100,   109,   109,   109,    96,   195,   186,   180,   185,   183,
     184,   116,   180
};

/* YYR1[RULE-NUM] -- Symbol kind of the left-hand side of rule RULE-NUM.  */
static const yytype_uint8 yyr1[] =
{
       0,   117,   118,   119,   119,   120,   120,   120,   121,   122,
     123,   124,   124,   124,   124,   124,   125,   125,   126,   126,
     126,   126,   126,   126,   126,   127,   127,   127,   128,   128,
     128,   128,   128,   128,   128,   128,   128,   128,   128,   128,
     128,   128,   129,   129,   129,   129,   129,   129,   129,   129,
     129,   130,   130,   131,   132,   133,   133,   133,   133,   134,
     134,   134,   134,   135,   136,   137,   137,   138,   138,   138,
     138,   139,   140,   140,   140,   140,   141,   141,   142,   143,
     144,   145,   145,   146,   146,   146,   146,   147,   148,   148,
     148,   148,   149,   149,   150,   151,   151,   152,   152,   153,
     153,   153,   154,   154,   155,   155,   156,   156,   157,   157,
     158,   158,   159,   160,   160,   161,   161,   162,   162,   162,
     163,   163,   163,   163,   163,   163,   163,   163,   164,   164,
     165,   166,   167,   167,   168,   168,   169,   169,   170,   170,
     170,   170,   171,   171,   171,   171,   171,   171,   171,   171,
     171,   171,   171,   171,   171,   171,   171,   171,   171,   171,
     171,   171,   171,   171,   171,   171,   171,   172,   172,   173,
     173,   174,   175,   175,   176,   176,   177,   177,   177,   177,
     178,   178,   179,   180,   180,   180,   181,   182,   182,   183,
     183,   184,   185,   186,   186,   187,   187,   187,   187,   188,
     189,   190,   191,   192,   193,   194,   195,   196,   196,   197,
     197
};

/* YYR2[RULE-NUM] -- Number of symbols on the right-hand side of rule RULE-NUM.  */
static const yytype_int8 yyr2[] =
{
       0,     2,     1,     2,     0,     3,     3,     3,     1,     1,
       1,     1,     1,     1,     2,     1,     1,     1,     2,     2,
       2,     2,     2,     2,     0,     2,     2,     0,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     0,     2,     2,     2,     2,     2,     2,     2,     2,
       0,     1,     0,     1,     1,     1,     1,     1,     0,     6,
       5,     4,     3,     4,     4,     2,     0,     1,     1,     1,
       1,     2,     3,     2,     4,     3,     2,     0,     5,     4,
       4,     2,     0,     1,     1,     1,     1,     2,     3,     2,
       4,     3,     2,     0,     5,     2,     4,     3,     1,     1,
       3,     4,     4,     2,     2,     0,     4,     4,     4,     2,
       2,     0,     4,     4,     2,     2,     0,     4,     5,     6,
       1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
       1,     6,     3,     0,     1,     0,     2,     2,     2,     2,
       1,     2,     1,     1,     1,     1,     1,     1,     1,     1,
       1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
       1,     1,     1,     1,     1,     1,     2,     1,     0,     3,
       1,     4,     3,     1,     2,     0,     1,     1,     1,     1,
       4,     1,     0,     4,     6,     0,     2,     1,     0,     3,
       1,     3,     0,     1,     1,     1,     1,     1,     1,     4,
       4,     4,     1,     1,     4,     1,     1,     1,     0,     1,
       0
};


enum { YYENOMEM = -2 };

#define yyerrok         (yyerrstatus = 0)
#define yyclearin       (yychar = YYEMPTY)

#define YYACCEPT        goto yyacceptlab
#define YYABORT         goto yyabortlab
#define YYERROR         goto yyerrorlab
#define YYNOMEM         goto yyexhaustedlab


#define YYRECOVERING()  (!!yyerrstatus)

#define YYBACKUP(Token, Value)                                    \
  do                                                              \
    if (yychar == YYEMPTY)                                        \
      {                                                           \
        yychar = (Token);                                         \
        yylval = (Value);                                         \
        YYPOPSTACK (yylen);                                       \
        yystate = *yyssp;                                         \
        goto yybackup;                                            \
      }                                                           \
    else                                                          \
      {                                                           \
        yyerror (YY_("syntax error: cannot back up")); \
        YYERROR;                                                  \
      }                                                           \
  while (0)

/* Backward compatibility with an undocumented macro.
   Use YYerror or YYUNDEF. */
#define YYERRCODE YYUNDEF


/* Enable debugging if requested.  */
#if YYDEBUG

# ifndef YYFPRINTF
#  include <stdio.h> /* INFRINGES ON USER NAME SPACE */
#  define YYFPRINTF fprintf
# endif

# define YYDPRINTF(Args)                        \
do {                                            \
  if (yydebug)                                  \
    YYFPRINTF Args;                             \
} while (0)




# define YY_SYMBOL_PRINT(Title, Kind, Value, Location)                    \
do {                                                                      \
  if (yydebug)                                                            \
    {                                                                     \
      YYFPRINTF (stderr, "%s ", Title);                                   \
      yy_symbol_print (stderr,                                            \
                  Kind, Value); \
      YYFPRINTF (stderr, "\n");                                           \
    }                                                                     \
} while (0)


/*-----------------------------------.
| Print this symbol's value on YYO.  |
`-----------------------------------*/

static void
yy_symbol_value_print (FILE *yyo,
                       yysymbol_kind_t yykind, YYSTYPE const * const yyvaluep)
{
  FILE *yyoutput = yyo;
  YY_USE (yyoutput);
  if (!yyvaluep)
    return;
  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  YY_USE (yykind);
  YY_IGNORE_MAYBE_UNINITIALIZED_END
}


/*---------------------------.
| Print this symbol on YYO.  |
`---------------------------*/

static void
yy_symbol_print (FILE *yyo,
                 yysymbol_kind_t yykind, YYSTYPE const * const yyvaluep)
{
  YYFPRINTF (yyo, "%s %s (",
             yykind < YYNTOKENS ? "token" : "nterm", yysymbol_name (yykind));

  yy_symbol_value_print (yyo, yykind, yyvaluep);
  YYFPRINTF (yyo, ")");
}

/*------------------------------------------------------------------.
| yy_stack_print -- Print the state stack from its BOTTOM up to its |
| TOP (included).                                                   |
`------------------------------------------------------------------*/

static void
yy_stack_print (yy_state_t *yybottom, yy_state_t *yytop)
{
  YYFPRINTF (stderr, "Stack now");
  for (; yybottom <= yytop; yybottom++)
    {
      int yybot = *yybottom;
      YYFPRINTF (stderr, " %d", yybot);
    }
  YYFPRINTF (stderr, "\n");
}

# define YY_STACK_PRINT(Bottom, Top)                            \
do {                                                            \
  if (yydebug)                                                  \
    yy_stack_print ((Bottom), (Top));                           \
} while (0)


/*------------------------------------------------.
| Report that the YYRULE is going to be reduced.  |
`------------------------------------------------*/

static void
yy_reduce_print (yy_state_t *yyssp, YYSTYPE *yyvsp,
                 int yyrule)
{
  int yylno = yyrline[yyrule];
  int yynrhs = yyr2[yyrule];
  int yyi;
  YYFPRINTF (stderr, "Reducing stack by rule %d (line %d):\n",
             yyrule - 1, yylno);
  /* The symbols being reduced.  */
  for (yyi = 0; yyi < yynrhs; yyi++)
    {
      YYFPRINTF (stderr, "   $%d = ", yyi + 1);
      yy_symbol_print (stderr,
                       YY_ACCESSING_SYMBOL (+yyssp[yyi + 1 - yynrhs]),
                       &yyvsp[(yyi + 1) - (yynrhs)]);
      YYFPRINTF (stderr, "\n");
    }
}

# define YY_REDUCE_PRINT(Rule)          \
do {                                    \
  if (yydebug)                          \
    yy_reduce_print (yyssp, yyvsp, Rule); \
} while (0)

/* Nonzero means print parse trace.  It is left uninitialized so that
   multiple parsers can coexist.  */
int yydebug;
#else /* !YYDEBUG */
# define YYDPRINTF(Args) ((void) 0)
# define YY_SYMBOL_PRINT(Title, Kind, Value, Location)
# define YY_STACK_PRINT(Bottom, Top)
# define YY_REDUCE_PRINT(Rule)
#endif /* !YYDEBUG */


/* YYINITDEPTH -- initial size of the parser's stacks.  */
#ifndef YYINITDEPTH
# define YYINITDEPTH 200
#endif

/* YYMAXDEPTH -- maximum size the stacks can grow to (effective only
   if the built-in stack extension method is used).

   Do not make this value too large; the results are undefined if
   YYSTACK_ALLOC_MAXIMUM < YYSTACK_BYTES (YYMAXDEPTH)
   evaluated with infinite-precision integer arithmetic.  */

#ifndef YYMAXDEPTH
# define YYMAXDEPTH 10000
#endif






/*-----------------------------------------------.
| Release the memory associated to this symbol.  |
`-----------------------------------------------*/

static void
yydestruct (const char *yymsg,
            yysymbol_kind_t yykind, YYSTYPE *yyvaluep)
{
  YY_USE (yyvaluep);
  if (!yymsg)
    yymsg = "Deleting";
  YY_SYMBOL_PRINT (yymsg, yykind, yyvaluep, yylocationp);

  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  YY_USE (yykind);
  YY_IGNORE_MAYBE_UNINITIALIZED_END
}






/*----------.
| yyparse.  |
`----------*/

int
yyparse (void)
{
/* Lookahead token kind.  */
int yychar;


/* The semantic value of the lookahead symbol.  */
/* Default value used for initialization, for pacifying older GCCs
   or non-GCC compilers.  */
YY_INITIAL_VALUE (static YYSTYPE yyval_default;)
YYSTYPE yylval YY_INITIAL_VALUE (= yyval_default);

    /* Number of syntax errors so far.  */
    int yynerrs = 0;

    yy_state_fast_t yystate = 0;
    /* Number of tokens to shift before error messages enabled.  */
    int yyerrstatus = 0;

    /* Refer to the stacks through separate pointers, to allow yyoverflow
       to reallocate them elsewhere.  */

    /* Their size.  */
    YYPTRDIFF_T yystacksize = YYINITDEPTH;

    /* The state stack: array, bottom, top.  */
    yy_state_t yyssa[YYINITDEPTH];
    yy_state_t *yyss = yyssa;
    yy_state_t *yyssp = yyss;

    /* The semantic value stack: array, bottom, top.  */
    YYSTYPE yyvsa[YYINITDEPTH];
    YYSTYPE *yyvs = yyvsa;
    YYSTYPE *yyvsp = yyvs;

  int yyn;
  /* The return value of yyparse.  */
  int yyresult;
  /* Lookahead symbol kind.  */
  yysymbol_kind_t yytoken = YYSYMBOL_YYEMPTY;
  /* The variables used to return semantic value and location from the
     action routines.  */
  YYSTYPE yyval;



#define YYPOPSTACK(N)   (yyvsp -= (N), yyssp -= (N))

  /* The number of symbols on the RHS of the reduced rule.
     Keep to zero when no symbol should be popped.  */
  int yylen = 0;

  YYDPRINTF ((stderr, "Starting parse\n"));

  yychar = YYEMPTY; /* Cause a token to be read.  */

  goto yysetstate;


/*------------------------------------------------------------.
| yynewstate -- push a new state, which is found in yystate.  |
`------------------------------------------------------------*/
yynewstate:
  /* In all cases, when you get here, the value and location stacks
     have just been pushed.  So pushing a state here evens the stacks.  */
  yyssp++;


/*--------------------------------------------------------------------.
| yysetstate -- set current state (the top of the stack) to yystate.  |
`--------------------------------------------------------------------*/
yysetstate:
  YYDPRINTF ((stderr, "Entering state %d\n", yystate));
  YY_ASSERT (0 <= yystate && yystate < YYNSTATES);
  YY_IGNORE_USELESS_CAST_BEGIN
  *yyssp = YY_CAST (yy_state_t, yystate);
  YY_IGNORE_USELESS_CAST_END
  YY_STACK_PRINT (yyss, yyssp);

  if (yyss + yystacksize - 1 <= yyssp)
#if !defined yyoverflow && !defined YYSTACK_RELOCATE
    YYNOMEM;
#else
    {
      /* Get the current used size of the three stacks, in elements.  */
      YYPTRDIFF_T yysize = yyssp - yyss + 1;

# if defined yyoverflow
      {
        /* Give user a chance to reallocate the stack.  Use copies of
           these so that the &'s don't force the real ones into
           memory.  */
        yy_state_t *yyss1 = yyss;
        YYSTYPE *yyvs1 = yyvs;

        /* Each stack pointer address is followed by the size of the
           data in use in that stack, in bytes.  This used to be a
           conditional around just the two extra args, but that might
           be undefined if yyoverflow is a macro.  */
        yyoverflow (YY_("memory exhausted"),
                    &yyss1, yysize * YYSIZEOF (*yyssp),
                    &yyvs1, yysize * YYSIZEOF (*yyvsp),
                    &yystacksize);
        yyss = yyss1;
        yyvs = yyvs1;
      }
# else /* defined YYSTACK_RELOCATE */
      /* Extend the stack our own way.  */
      if (YYMAXDEPTH <= yystacksize)
        YYNOMEM;
      yystacksize *= 2;
      if (YYMAXDEPTH < yystacksize)
        yystacksize = YYMAXDEPTH;

      {
        yy_state_t *yyss1 = yyss;
        union yyalloc *yyptr =
          YY_CAST (union yyalloc *,
                   YYSTACK_ALLOC (YY_CAST (YYSIZE_T, YYSTACK_BYTES (yystacksize))));
        if (! yyptr)
          YYNOMEM;
        YYSTACK_RELOCATE (yyss_alloc, yyss);
        YYSTACK_RELOCATE (yyvs_alloc, yyvs);
#  undef YYSTACK_RELOCATE
        if (yyss1 != yyssa)
          YYSTACK_FREE (yyss1);
      }
# endif

      yyssp = yyss + yysize - 1;
      yyvsp = yyvs + yysize - 1;

      YY_IGNORE_USELESS_CAST_BEGIN
      YYDPRINTF ((stderr, "Stack size increased to %ld\n",
                  YY_CAST (long, yystacksize)));
      YY_IGNORE_USELESS_CAST_END

      if (yyss + yystacksize - 1 <= yyssp)
        YYABORT;
    }
#endif /* !defined yyoverflow && !defined YYSTACK_RELOCATE */


  if (yystate == YYFINAL)
    YYACCEPT;

  goto yybackup;


/*-----------.
| yybackup.  |
`-----------*/
yybackup:
  /* Do appropriate processing given the current state.  Read a
     lookahead token if we need one and don't already have one.  */

  /* First try to decide what to do without reference to lookahead token.  */
  yyn = yypact[yystate];
  if (yypact_value_is_default (yyn))
    goto yydefault;

  /* Not known => get a lookahead token if don't already have one.  */

  /* YYCHAR is either empty, or end-of-input, or a valid lookahead.  */
  if (yychar == YYEMPTY)
    {
      YYDPRINTF ((stderr, "Reading a token\n"));
      yychar = yylex (&yylval);
    }

  if (yychar <= YYEOF)
    {
      yychar = YYEOF;
      yytoken = YYSYMBOL_YYEOF;
      YYDPRINTF ((stderr, "Now at end of input.\n"));
    }
  else if (yychar == YYerror)
    {
      /* The scanner already issued an error message, process directly
         to error recovery.  But do not keep the error token as
         lookahead, it is too special and may lead us to an endless
         loop in error recovery. */
      yychar = YYUNDEF;
      yytoken = YYSYMBOL_YYerror;
      goto yyerrlab1;
    }
  else
    {
      yytoken = YYTRANSLATE (yychar);
      YY_SYMBOL_PRINT ("Next token is", yytoken, &yylval, &yylloc);
    }

  /* If the proper action on seeing token YYTOKEN is to reduce or to
     detect an error, take that action.  */
  yyn += yytoken;
  if (yyn < 0 || YYLAST < yyn || yycheck[yyn] != yytoken)
    goto yydefault;
  yyn = yytable[yyn];
  if (yyn <= 0)
    {
      if (yytable_value_is_error (yyn))
        goto yyerrlab;
      yyn = -yyn;
      goto yyreduce;
    }

  /* Count tokens shifted since error; after three, turn off error
     status.  */
  if (yyerrstatus)
    yyerrstatus--;

  /* Shift the lookahead token.  */
  YY_SYMBOL_PRINT ("Shifting", yytoken, &yylval, &yylloc);
  yystate = yyn;
  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  *++yyvsp = yylval;
  YY_IGNORE_MAYBE_UNINITIALIZED_END

  /* Discard the shifted token.  */
  yychar = YYEMPTY;
  goto yynewstate;


/*-----------------------------------------------------------.
| yydefault -- do the default action for the current state.  |
`-----------------------------------------------------------*/
yydefault:
  yyn = yydefact[yystate];
  if (yyn == 0)
    goto yyerrlab;
  goto yyreduce;


/*-----------------------------.
| yyreduce -- do a reduction.  |
`-----------------------------*/
yyreduce:
  /* yyn is the number of a rule to reduce with.  */
  yylen = yyr2[yyn];

  /* If YYLEN is nonzero, implement the default value of the action:
     '$$ = $1'.

     Otherwise, the following line sets YYVAL to garbage.
     This behavior is undocumented and Bison
     users should not rely upon it.  Assigning to YYVAL
     unconditionally makes the parser a bit smaller, and it avoids a
     GCC warning that YYVAL may be used uninitialized.  */
  yyval = yyvsp[1-yylen];


  YY_REDUCE_PRINT (yyn);
  switch (yyn)
    {
  case 8: /* file_import_flag: import_flag  */
#line 286 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                { GFileContext->SetGlobalFlags( (yyvsp[0].m_flags) ); }
#line 1724 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 9: /* file_importonly_flag: importonly_flag  */
#line 290 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                { GFileContext->SetGlobalFlags( (yyvsp[0].m_flags) ); }
#line 1730 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 10: /* file_holy_flag: holy_flag  */
#line 294 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                        { GFileContext->SetGlobalFlags( (yyvsp[0].m_flags) ); }
#line 1736 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 18: /* class_flags: TOKEN_ABSTRACT class_flags  */
#line 315 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                        { (yyval.m_flags) = SSF_Abstract | (yyvsp[0].m_flags); }
#line 1742 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 19: /* class_flags: TOKEN_PRIVATE class_flags  */
#line 316 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                        { (yyval.m_flags) = SSF_Private | (yyvsp[0].m_flags); }
#line 1748 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 20: /* class_flags: TOKEN_PROTECTED class_flags  */
#line 317 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                        { (yyval.m_flags) = SSF_Protected | (yyvsp[0].m_flags); }
#line 1754 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 21: /* class_flags: TOKEN_PUBLIC class_flags  */
#line 318 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                        { (yyval.m_flags) = SSF_Public | (yyvsp[0].m_flags); }
#line 1760 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 22: /* class_flags: TOKEN_FINAL class_flags  */
#line 319 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                        { (yyval.m_flags) = SSF_Final | (yyvsp[0].m_flags); }
#line 1766 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 23: /* class_flags: TOKEN_TESTONLY class_flags  */
#line 320 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                        { (yyval.m_flags) = SSF_TestOnly | (yyvsp[0].m_flags); }
#line 1772 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 24: /* class_flags: %empty  */
#line 321 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                                { (yyval.m_flags) = 0; }
#line 1778 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 25: /* struct_flags: TOKEN_FINAL struct_flags  */
#line 325 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                { (yyval.m_flags) = SSF_Final | (yyvsp[0].m_flags); }
#line 1784 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 26: /* struct_flags: TOKEN_TESTONLY struct_flags  */
#line 326 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                { (yyval.m_flags) = SSF_TestOnly | (yyvsp[0].m_flags); }
#line 1790 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 27: /* struct_flags: %empty  */
#line 327 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                                { (yyval.m_flags) = 0; }
#line 1796 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 28: /* function_flags: TOKEN_EXEC function_flags  */
#line 331 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                { (yyval.m_flags) = SSF_Exec | (yyvsp[0].m_flags); }
#line 1802 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 29: /* function_flags: TOKEN_TIMER function_flags  */
#line 332 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                { (yyval.m_flags) = SSF_Timer | (yyvsp[0].m_flags); }
#line 1808 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 30: /* function_flags: TOKEN_STATIC function_flags  */
#line 333 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                { (yyval.m_flags) = SSF_Static | (yyvsp[0].m_flags); }
#line 1814 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 31: /* function_flags: TOKEN_MULTICAST function_flags  */
#line 334 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                { (yyval.m_flags) = SSF_Multicast | (yyvsp[0].m_flags); }
#line 1820 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 32: /* function_flags: TOKEN_HOST function_flags  */
#line 335 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                { (yyval.m_flags) = SSF_Host | (yyvsp[0].m_flags); }
#line 1826 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 33: /* function_flags: TOKEN_CLIENT function_flags  */
#line 336 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                { (yyval.m_flags) = SSF_Client | (yyvsp[0].m_flags); }
#line 1832 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 34: /* function_flags: TOKEN_RELIABLE function_flags  */
#line 337 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                { (yyval.m_flags) = SSF_Reliable | (yyvsp[0].m_flags); }
#line 1838 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 35: /* function_flags: TOKEN_VIRTUAL function_flags  */
#line 338 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                { (yyval.m_flags) = SSF_Virtual | (yyvsp[0].m_flags); }
#line 1844 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 36: /* function_flags: TOKEN_OVERRIDE function_flags  */
#line 339 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                { (yyval.m_flags) = SSF_Override | (yyvsp[0].m_flags); }
#line 1850 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 37: /* function_flags: TOKEN_CONST function_flags  */
#line 340 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                { (yyval.m_flags) = SSF_Const | (yyvsp[0].m_flags); }
#line 1856 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 38: /* function_flags: TOKEN_QUEST function_flags  */
#line 341 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                { (yyval.m_flags) = SSF_Quest | (yyvsp[0].m_flags); }
#line 1862 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 39: /* function_flags: TOKEN_FINAL function_flags  */
#line 342 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                { (yyval.m_flags) = SSF_Final | (yyvsp[0].m_flags); }
#line 1868 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 40: /* function_flags: TOKEN_TESTONLY function_flags  */
#line 343 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                { (yyval.m_flags) = SSF_TestOnly | (yyvsp[0].m_flags); }
#line 1874 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 41: /* function_flags: %empty  */
#line 344 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                                { (yyval.m_flags) = 0; }
#line 1880 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 42: /* var_flags: TOKEN_CONST var_flags  */
#line 348 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                                { (yyval.m_flags) = SSF_Const | (yyvsp[0].m_flags); }
#line 1886 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 43: /* var_flags: TOKEN_MUTABLE var_flags  */
#line 349 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                                { (yyval.m_flags) = SSF_Mutable | (yyvsp[0].m_flags); }
#line 1892 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 44: /* var_flags: TOKEN_EDITABLE var_flags  */
#line 350 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                                { (yyval.m_flags) = SSF_Editable | (yyvsp[0].m_flags); }
#line 1898 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 45: /* var_flags: TOKEN_INSTANCE_EDITABLE var_flags  */
#line 351 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                        { (yyval.m_flags) = SSF_InstanceEditable | (yyvsp[0].m_flags); }
#line 1904 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 46: /* var_flags: TOKEN_REPLICATED var_flags  */
#line 352 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                                { (yyval.m_flags) = SSF_Replicated | (yyvsp[0].m_flags); }
#line 1910 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 47: /* var_flags: TOKEN_INLINED var_flags  */
#line 353 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                                { (yyval.m_flags) = SSF_Inlined | (yyvsp[0].m_flags); }
#line 1916 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 48: /* var_flags: TOKEN_PERSISTENT var_flags  */
#line 354 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                                { (yyval.m_flags) = SSF_Persistent | (yyvsp[0].m_flags); }
#line 1922 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 49: /* var_flags: TOKEN_TESTONLY var_flags  */
#line 355 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                                { (yyval.m_flags) = SSF_TestOnly | (yyvsp[0].m_flags); }
#line 1928 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 50: /* var_flags: %empty  */
#line 356 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                                        { (yyval.m_flags) = 0; }
#line 1934 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 51: /* import_flag: TOKEN_IMPORT  */
#line 364 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                        { (yyval.m_flags) = SSF_Import; }
#line 1940 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 52: /* import_flag: %empty  */
#line 365 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                        { (yyval.m_flags) = 0; }
#line 1946 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 53: /* importonly_flag: TOKEN_IMPORTONLY  */
#line 369 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                        { (yyval.m_flags) = SSF_ImportOnly; }
#line 1952 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 54: /* holy_flag: TOKEN_HOLY  */
#line 373 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                        { (yyval.m_flags) = SSF_ImportOnly | SSF_Final; }
#line 1958 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 55: /* access_flag: TOKEN_PRIVATE  */
#line 377 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                        { (yyval.m_flags) = SSF_Private; }
#line 1964 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 56: /* access_flag: TOKEN_PROTECTED  */
#line 378 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                        { (yyval.m_flags) = SSF_Protected; }
#line 1970 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 57: /* access_flag: TOKEN_PUBLIC  */
#line 379 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                        { (yyval.m_flags) = SSF_Public; }
#line 1976 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 58: /* access_flag: %empty  */
#line 380 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                        { (yyval.m_flags) = 0; }
#line 1982 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 59: /* attributes: '[' ident '=' default_val_const ']' attributes  */
#line 385 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
        {
		GFileContext->AddAttribute( ScriptAttribute( (yyvsp[-4].m_token), (yyvsp[-2].m_token), (yyvsp[-5].m_context) ) );
	}
#line 1990 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 60: /* attributes: '[' ident '=' default_val_const ']'  */
#line 389 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
        {
		GFileContext->AddAttribute( ScriptAttribute( (yyvsp[-3].m_token), (yyvsp[-1].m_token), (yyvsp[-4].m_context) ) );
	}
#line 1998 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 61: /* attributes: '[' ident ']' attributes  */
#line 393 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
        {
		GFileContext->AddAttribute( ScriptAttribute( (yyvsp[-2].m_token), red::StringView(), (yyvsp[-3].m_context) ) );
	}
#line 2006 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 62: /* attributes: '[' ident ']'  */
#line 397 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
        {
		GFileContext->AddAttribute( ScriptAttribute( (yyvsp[-1].m_token), red::StringView(), (yyvsp[-2].m_context) ) );
	}
#line 2014 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 63: /* class_dcl: class_header '{' class_elem_list '}'  */
#line 407 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                               { GFileParser->EndDefinition( GFileContext->GetCurrentTokenLine(), (yyvsp[-2].m_context), (yyvsp[0].m_context) ); GFileParser->PopContext(); }
#line 2020 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 64: /* class_header: class_flags TOKEN_CLASS ident class_extends  */
#line 411 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                      { GFileParser->StartClass( (yyvsp[-1].m_context), (yyvsp[0].m_context), (yyvsp[-1].m_string), (yyvsp[0].m_string), (yyvsp[-3].m_flags) | GFileContext->GetGlobalFlags() ); }
#line 2026 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 71: /* class_access_and_import_flags: access_flag import_flag  */
#line 427 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                { GFileContext->SetGlobalFlags( (yyvsp[-1].m_flags) | (yyvsp[0].m_flags) ); }
#line 2032 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 76: /* class_extends: TOKEN_EXTENDS ident  */
#line 438 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                { (yyval.m_string) = (yyvsp[0].m_string); (yyval.m_context) = (yyvsp[0].m_context); }
#line 2038 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 77: /* class_extends: %empty  */
#line 439 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                        { (yyval.m_string) = (""); (yyval.m_context) = nullptr; }
#line 2044 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 78: /* var_dcl: var_flags TOKEN_VAR function_param_ident_list ':' full_type  */
#line 450 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                {
			const YYSTYPE_File::Idents& idents = (yyvsp[-2].m_idents);
			const Uint32 identsCount = idents.Size();

			const ScriptAttributes& attributes = GFileContext->GetAttributes();

			for ( Uint32 i = 0; i < identsCount; ++i )
			{
				GFileParser->AddProperty( idents[i].m_context, (yyvsp[0].m_context), idents[i].m_value, (yyvsp[-4].m_flags) | GFileContext->GetGlobalFlags(), (yyvsp[0].m_typeName), false, attributes );
			}

			GFileContext->ClearAttributes();
		}
#line 2062 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 79: /* struct_dcl: struct_info '{' struct_elem_list '}'  */
#line 470 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                        { GFileParser->EndDefinition( GFileContext->GetCurrentTokenLine(), (yyvsp[-2].m_context), (yyvsp[0].m_context) ); GFileParser->PopContext(); }
#line 2068 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 80: /* struct_info: struct_flags TOKEN_STRUCT ident struct_extends  */
#line 474 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                         { GFileParser->StartStruct( (yyvsp[-1].m_context), (yyvsp[0].m_context), (yyvsp[-1].m_string), (yyvsp[0].m_string), (yyvsp[-3].m_flags) | GFileContext->GetGlobalFlags() ); }
#line 2074 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 87: /* struct_access_and_import_flags: access_flag import_flag  */
#line 490 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                { GFileContext->SetGlobalFlags( (yyvsp[-1].m_flags) | (yyvsp[0].m_flags) ); }
#line 2080 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 92: /* struct_extends: TOKEN_EXTENDS ident  */
#line 501 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                { (yyval.m_string) = (yyvsp[0].m_string); (yyval.m_context) = (yyvsp[0].m_context); }
#line 2086 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 93: /* struct_extends: %empty  */
#line 502 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                        { (yyval.m_string) = (""); (yyval.m_context) = nullptr; }
#line 2092 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 94: /* enum_dcl: enum_info '{' enum_name_list optional_comma '}'  */
#line 511 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                                { GFileParser->EndDefinition( GFileContext->GetCurrentTokenLine(), (yyvsp[-3].m_context), (yyvsp[0].m_context) ); GFileParser->PopContext(); }
#line 2098 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 95: /* enum_info: TOKEN_ENUM ident  */
#line 515 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                           { GFileParser->StartEnum( (yyvsp[0].m_context), (yyvsp[0].m_string), GFileContext->GetGlobalFlags() ); }
#line 2104 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 96: /* enum_info: TOKEN_ENUM ident ':' ident  */
#line 517 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
        {
		(yyval.m_typeName) = CScriptTypeDummy::MakeSimple( (yyvsp[0].m_string) );
		GFileParser->StartEnum( (yyvsp[-2].m_context), (yyvsp[-2].m_string), (yyval.m_typeName), GFileContext->GetGlobalFlags() );
	}
#line 2113 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 97: /* enum_name_list: enum_name_list ',' enum_name  */
#line 524 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                        {}
#line 2119 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 98: /* enum_name_list: enum_name  */
#line 525 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                                {}
#line 2125 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 99: /* enum_name: ident  */
#line 529 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                        { GFileParser->AddEnumOption( (yyvsp[0].m_context), (yyvsp[0].m_string) ); }
#line 2131 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 100: /* enum_name: ident '=' integer  */
#line 530 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                { GFileParser->AddEnumOption( (yyvsp[-2].m_context), (yyvsp[-2].m_string), (yyvsp[0].m_integer) ); }
#line 2137 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 101: /* enum_name: ident '=' '-' integer  */
#line 531 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                        { GFileParser->AddEnumOption( (yyvsp[-3].m_context), (yyvsp[-3].m_string), -(yyvsp[0].m_integer) ); }
#line 2143 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 106: /* browsable_val: ident '=' TOKEN_BOOL_TRUE semicolon  */
#line 548 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                                { GFileParser->SetBrowsable( (yyvsp[-3].m_context), (yyvsp[-3].m_string), true ); }
#line 2149 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 107: /* browsable_val: ident '=' TOKEN_BOOL_FALSE semicolon  */
#line 549 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                                { GFileParser->SetBrowsable( (yyvsp[-3].m_context), (yyvsp[-3].m_string), false ); }
#line 2155 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 112: /* hint_val: ident '=' string semicolon  */
#line 567 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                                        { GFileParser->AddHint( (yyvsp[-3].m_context), (yyvsp[-3].m_string), (yyvsp[-1].m_string) ); }
#line 2161 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 117: /* default_val: ident '=' default_val_const semicolon  */
#line 585 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                                                { GFileParser->AddDefaultValue( (yyvsp[-3].m_context), (yyvsp[-3].m_string), (yyvsp[-1].m_token), false ); }
#line 2167 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 118: /* default_val: ident '=' '-' default_arithmetic_val_const semicolon  */
#line 586 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                                { GFileParser->AddDefaultValue( (yyvsp[-4].m_context), (yyvsp[-4].m_string), (yyvsp[-1].m_token), true ); }
#line 2173 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 119: /* default_val: ident '=' TOKEN_IDENT '.' TOKEN_IDENT semicolon  */
#line 588 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
        {
		red::String tmp = (yyvsp[-3].m_token).ToString().TrimLeft().TrimRight() + (yyvsp[-2].m_token).ToString() + (yyvsp[-1].m_token).ToString().TrimRight();
		red::StringView view{ tmp };
		GFileParser->AddDefaultValue( (yyvsp[-5].m_context), (yyvsp[-5].m_string), view, false );
	}
#line 2183 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 120: /* default_val_const: TOKEN_INTEGER  */
#line 596 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                                                        { (yyval.m_token) = (yyvsp[0].m_token); }
#line 2189 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 121: /* default_val_const: TOKEN_FLOAT  */
#line 597 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                                                        { (yyval.m_token) = (yyvsp[0].m_token); }
#line 2195 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 122: /* default_val_const: TOKEN_STRING  */
#line 598 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                                                        { (yyval.m_token) = (yyvsp[0].m_token); }
#line 2201 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 123: /* default_val_const: TOKEN_NAME  */
#line 599 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                                                        { (yyval.m_token) = (yyvsp[0].m_token); }
#line 2207 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 124: /* default_val_const: TOKEN_TDBID  */
#line 600 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                                                        { (yyval.m_token) = (yyvsp[0].m_token); }
#line 2213 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 125: /* default_val_const: TOKEN_RESREF  */
#line 601 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                                                        { (yyval.m_token) = (yyvsp[0].m_token); }
#line 2219 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 126: /* default_val_const: TOKEN_BOOL_TRUE  */
#line 602 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                                                        { (yyval.m_token) = (yyvsp[0].m_token); }
#line 2225 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 127: /* default_val_const: TOKEN_BOOL_FALSE  */
#line 603 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                                                        { (yyval.m_token) = (yyvsp[0].m_token); }
#line 2231 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 128: /* default_arithmetic_val_const: TOKEN_INTEGER  */
#line 607 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                                                        { (yyval.m_token) = (yyvsp[0].m_token); }
#line 2237 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 129: /* default_arithmetic_val_const: TOKEN_FLOAT  */
#line 608 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                                                        { (yyval.m_token) = (yyvsp[0].m_token); }
#line 2243 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 132: /* function_ret_value: ':' function_ret_value_flag obj_type  */
#line 624 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                        { GFileParser->SetReturnValue( (yyvsp[0].m_context), (yyvsp[0].m_typeName), (yyvsp[-1].m_flags) ); }
#line 2249 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 134: /* function_ret_value_flag: TOKEN_CONST  */
#line 629 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                        { (yyval.m_flags) = SSF_Const; }
#line 2255 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 135: /* function_ret_value_flag: %empty  */
#line 630 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                        { (yyval.m_flags) = 0; }
#line 2261 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 136: /* function_inner_header: function_flags function_name  */
#line 635 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
        {
		const ScriptAttributes& attributes = GFileContext->GetAttributes();
		GFileParser->StartFunction( (yyvsp[0].m_context), (yyvsp[0].m_string), (yyvsp[-1].m_flags) | (yyvsp[0].m_flags) | GFileContext->GetGlobalFlags(), attributes );
		GFileContext->ClearAttributes();
	}
#line 2271 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 137: /* function_inner_header: TOKEN_EVENT ident  */
#line 641 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
        {
		const ScriptAttributes& attributes = GFileContext->GetAttributes();
		GFileParser->StartFunction( (yyvsp[0].m_context), (yyvsp[0].m_string), SSF_Event | SSF_Virtual | SSF_Protected, attributes );
		GFileContext->ClearAttributes();
	}
#line 2281 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 138: /* function_name: TOKEN_FUNCTION ident  */
#line 649 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                        { (yyval.m_string) = (yyvsp[0].m_string); (yyval.m_context) = (yyvsp[0].m_context); }
#line 2287 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 139: /* function_name: TOKEN_OPERATOR function_op_name  */
#line 650 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                { (yyval.m_string) = (yyvsp[0].m_string); (yyval.m_flags) = SSF_Operator; (yyval.m_context) = (yyvsp[-1].m_context); }
#line 2293 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 140: /* function_name: TOKEN_CAST  */
#line 651 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                                { (yyval.m_string) = ("Cast"); (yyval.m_flags) = SSF_Cast; (yyval.m_context) = (yyvsp[0].m_context); }
#line 2299 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 141: /* function_name: TOKEN_IMPLICIT TOKEN_CAST  */
#line 652 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                        { (yyval.m_string) = ("Cast"); (yyval.m_flags) = SSF_Cast | SSF_Implicit; (yyval.m_context) = (yyvsp[-1].m_context); }
#line 2305 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 142: /* function_op_name: TOKEN_OP_IADD  */
#line 656 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                { (yyval.m_string) = red::StringView( ("OperatorAssignAdd" ) ); }
#line 2311 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 143: /* function_op_name: TOKEN_OP_ISUB  */
#line 657 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                { (yyval.m_string) = red::StringView( ("OperatorAssignSubtract") ); }
#line 2317 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 144: /* function_op_name: TOKEN_OP_IMUL  */
#line 658 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                { (yyval.m_string) = red::StringView( ("OperatorAssignMultiply") ); }
#line 2323 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 145: /* function_op_name: TOKEN_OP_IDIV  */
#line 659 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                { (yyval.m_string) = red::StringView( ("OperatorAssignDivide") ); }
#line 2329 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 146: /* function_op_name: TOKEN_OP_IAND  */
#line 660 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                { (yyval.m_string) = red::StringView( ("OperatorAssignAnd") ); }
#line 2335 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 147: /* function_op_name: TOKEN_OP_IOR  */
#line 661 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                { (yyval.m_string) = red::StringView( ("OperatorAssignOr") ); }
#line 2341 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 148: /* function_op_name: TOKEN_OP_LOGIC_OR  */
#line 662 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                { (yyval.m_string) = red::StringView( ("OperatorLogicOr") ); }
#line 2347 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 149: /* function_op_name: TOKEN_OP_LOGIC_AND  */
#line 663 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                        { (yyval.m_string) = red::StringView( ("OperatorLogicAnd") ); }
#line 2353 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 150: /* function_op_name: '|'  */
#line 664 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                        { (yyval.m_string) = red::StringView( ("OperatorOr") ); }
#line 2359 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 151: /* function_op_name: '&'  */
#line 665 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                        { (yyval.m_string) = red::StringView( ("OperatorAnd") ); }
#line 2365 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 152: /* function_op_name: '^'  */
#line 666 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                        { (yyval.m_string) = red::StringView( ("OperatorXor") ); }
#line 2371 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 153: /* function_op_name: TOKEN_OP_EQUAL  */
#line 667 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                { (yyval.m_string) = red::StringView( ("OperatorEqual") ); }
#line 2377 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 154: /* function_op_name: TOKEN_OP_NOTEQUAL  */
#line 668 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                { (yyval.m_string) = red::StringView( ("OperatorNotEqual") ); }
#line 2383 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 155: /* function_op_name: '<'  */
#line 669 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                        { (yyval.m_string) = red::StringView( ("OperatorLess") ); }
#line 2389 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 156: /* function_op_name: '>'  */
#line 670 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                        { (yyval.m_string) = red::StringView( ("OperatorGreater") ); }
#line 2395 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 157: /* function_op_name: TOKEN_OP_GREQ  */
#line 671 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                { (yyval.m_string) = red::StringView( ("OperatorGreaterEqual") ); }
#line 2401 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 158: /* function_op_name: TOKEN_OP_LEEQ  */
#line 672 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                { (yyval.m_string) = red::StringView( ("OperatorLessEqual") ); }
#line 2407 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 159: /* function_op_name: '+'  */
#line 673 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                        { (yyval.m_string) = red::StringView( ("OperatorAdd") ); }
#line 2413 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 160: /* function_op_name: '*'  */
#line 674 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                        { (yyval.m_string) = red::StringView( ("OperatorMultiply") ); }
#line 2419 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 161: /* function_op_name: '/'  */
#line 675 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                        { (yyval.m_string) = red::StringView( ("OperatorDivide") ); }
#line 2425 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 162: /* function_op_name: '%'  */
#line 676 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                        { (yyval.m_string) = red::StringView( ("OperatorModulo") ); }
#line 2431 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 163: /* function_op_name: '!'  */
#line 677 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                        { (yyval.m_string) = red::StringView( ("OperatorLogicNot") ); }
#line 2437 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 164: /* function_op_name: '~'  */
#line 678 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                        { (yyval.m_string) = red::StringView( ("OperatorBitNot") ); }
#line 2443 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 165: /* function_op_name: '-'  */
#line 679 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                        { (yyval.m_string) = red::StringView( ("OperatorNeg") ); }
#line 2449 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 166: /* function_op_name: '[' ']'  */
#line 680 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                        { (yyval.m_string) = red::StringView( ("OperatorArray") ); }
#line 2455 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 171: /* function_param: function_param_flag_list function_param_ident_list ':' obj_type  */
#line 695 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                {
			const YYSTYPE_File::Idents& idents = (yyvsp[-2].m_idents);
			const Uint32 identsCount = idents.Size();
			for ( Uint32 i = 0; i < identsCount; ++i )
			{
				GFileParser->AddProperty( idents[i].m_context, (yyvsp[0].m_context), idents[i].m_value, (yyvsp[-3].m_flags), (yyvsp[0].m_typeName), true );
			}
		}
#line 2468 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 172: /* function_param_ident_list: ident ',' function_param_ident_list  */
#line 706 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                { (yyval.m_idents).PushBack( YYSTYPE_File::Ident( (yyvsp[-2].m_string), (yyvsp[-2].m_context) ) ); (yyval.m_idents).PushBack( (yyvsp[0].m_idents) );	}
#line 2474 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 173: /* function_param_ident_list: ident  */
#line 707 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                                                { (yyval.m_idents).PushBack( YYSTYPE_File::Ident( (yyvsp[0].m_string), (yyvsp[0].m_context) ) ); }
#line 2480 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 174: /* function_param_flag_list: function_param_flag function_param_flag_list  */
#line 711 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                           { (yyval.m_flags) = (yyvsp[-1].m_flags) | (yyvsp[0].m_flags); }
#line 2486 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 175: /* function_param_flag_list: %empty  */
#line 712 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                           { (yyval.m_flags) = 0; }
#line 2492 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 176: /* function_param_flag: TOKEN_OUT  */
#line 716 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                   { (yyval.m_flags) = SSF_Out; }
#line 2498 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 177: /* function_param_flag: TOKEN_SKIP  */
#line 717 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                   { (yyval.m_flags) = SSF_Skipable; }
#line 2504 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 178: /* function_param_flag: TOKEN_OPTIONAL  */
#line 718 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                   { (yyval.m_flags) = SSF_Optional; }
#line 2510 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 179: /* function_param_flag: TOKEN_CONST  */
#line 719 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                           { (yyval.m_flags) = SSF_Const; }
#line 2516 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 180: /* function_tail: '{' function_local_vars_dcl function_body '}'  */
#line 723 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                                { GFileParser->EndDefinition( GFileContext->GetCurrentTokenLine(), (yyvsp[-3].m_context), (yyvsp[0].m_context) ); GFileParser->PopContext(); }
#line 2522 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 181: /* function_tail: semicolon  */
#line 724 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                                                                        { GFileParser->SetFunctionUndefined(); GFileParser->PopContext(); }
#line 2528 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 182: /* function_body: %empty  */
#line 728 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                      { GFileContext->ExtractFunctionCode();  yyclearin; }
#line 2534 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 186: /* function_local_vars_header: function_local_var_flag TOKEN_VAR  */
#line 738 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                { GFileParser->ResetFunctionPropertyList(); GFileParser->SetFunctionLocalParamFlags( (yyvsp[-1].m_flags) ); }
#line 2540 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 187: /* function_local_var_flag: TOKEN_CONST  */
#line 742 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                        { (yyval.m_flags) = SSF_Const; }
#line 2546 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 188: /* function_local_var_flag: %empty  */
#line 743 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                        { (yyval.m_flags) = 0; }
#line 2552 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 191: /* function_local_var_single_dcl: function_param_ident_list ':' obj_type  */
#line 753 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
        {
		const YYSTYPE_File::Idents& idents = (yyvsp[-2].m_idents);
		const Uint32 identsCount = idents.Size();
		for( Uint32 i = 0; i < identsCount; ++i )
		{
			GFileParser->AddProperty( idents[ i ].m_context, (yyvsp[0].m_context), idents[ i ].m_value, GFileParser->GetFunctionLocalParamFlags(), (yyvsp[0].m_typeName), false );
		}
	}
#line 2565 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 192: /* function_local_var_initializer: %empty  */
#line 763 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                      { CScriptTokenStream initCode; GFileContext->ExtractInitCode( initCode ); GFileParser->SetLastFunctionPropertyInitCode( initCode ); yyclearin; }
#line 2571 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 195: /* full_type: array_type  */
#line 776 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                { (yyval.m_typeName) = (yyvsp[0].m_typeName); }
#line 2577 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 196: /* full_type: static_array_type  */
#line 777 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                { (yyval.m_typeName) = (yyvsp[0].m_typeName); }
#line 2583 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 197: /* full_type: weak_type  */
#line 778 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                        { (yyval.m_typeName) = (yyvsp[0].m_typeName); }
#line 2589 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 198: /* full_type: ident  */
#line 779 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                        { (yyval.m_typeName) = CScriptTypeDummy::MakeSimple( (yyvsp[0].m_string) ); }
#line 2595 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 199: /* ref_type: TOKEN_REF '<' full_type '>'  */
#line 783 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                        { (yyval.m_typeName) = CScriptTypeDummy::MakeReference( (yyvsp[-1].m_typeName) ); }
#line 2601 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 200: /* array_type: TOKEN_ARRAY '<' full_type '>'  */
#line 787 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                        { (yyval.m_typeName) = CScriptTypeDummy::MakeDynamicArray( (yyvsp[-1].m_typeName) ); }
#line 2607 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 201: /* static_array_type: full_type '[' integer ']'  */
#line 791 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                                { (yyval.m_typeName) = CScriptTypeDummy::MakeStaticArray( (yyvsp[-3].m_typeName), (yyvsp[-1].m_integer) ); }
#line 2613 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 202: /* integer: TOKEN_INTEGER  */
#line 796 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
        {
		if ( !red::StringToInt( (yyval.m_integer), (yyvsp[0].m_token).ToString().AsChar(), nullptr, red::BaseAuto ) )
		{
			yyerror( "Could not convert script token into integer" );
		}
		else
		{
			(yyval.m_string) = (yyvsp[0].m_token); (yyval.m_context) = (yyvsp[0].m_context);
		}
	}
#line 2628 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 203: /* string: TOKEN_STRING  */
#line 809 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                { (yyval.m_string) = (yyvsp[0].m_token); (yyval.m_context) = (yyvsp[0].m_context); }
#line 2634 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 204: /* weak_type: TOKEN_WEAK '<' full_type '>'  */
#line 813 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                       { (yyval.m_typeName) = CScriptTypeDummy::MakeWeakHandle( (yyvsp[-1].m_typeName) ); }
#line 2640 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;

  case 205: /* ident: TOKEN_IDENT  */
#line 817 "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\scriptCompiler\\src\\scriptFileParser.bison"
                                { (yyval.m_string) = (yyvsp[0].m_token); (yyval.m_context) = (yyvsp[0].m_context); }
#line 2646 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"
    break;


#line 2650 "D:\\cp2077 port\\generated\\redReflection\\bison_unused.cxx"

      default: break;
    }
  /* User semantic actions sometimes alter yychar, and that requires
     that yytoken be updated with the new translation.  We take the
     approach of translating immediately before every use of yytoken.
     One alternative is translating here after every semantic action,
     but that translation would be missed if the semantic action invokes
     YYABORT, YYACCEPT, or YYERROR immediately after altering yychar or
     if it invokes YYBACKUP.  In the case of YYABORT or YYACCEPT, an
     incorrect destructor might then be invoked immediately.  In the
     case of YYERROR or YYBACKUP, subsequent parser actions might lead
     to an incorrect destructor call or verbose syntax error message
     before the lookahead is translated.  */
  YY_SYMBOL_PRINT ("-> $$ =", YY_CAST (yysymbol_kind_t, yyr1[yyn]), &yyval, &yyloc);

  YYPOPSTACK (yylen);
  yylen = 0;

  *++yyvsp = yyval;

  /* Now 'shift' the result of the reduction.  Determine what state
     that goes to, based on the state we popped back to and the rule
     number reduced by.  */
  {
    const int yylhs = yyr1[yyn] - YYNTOKENS;
    const int yyi = yypgoto[yylhs] + *yyssp;
    yystate = (0 <= yyi && yyi <= YYLAST && yycheck[yyi] == *yyssp
               ? yytable[yyi]
               : yydefgoto[yylhs]);
  }

  goto yynewstate;


/*--------------------------------------.
| yyerrlab -- here on detecting error.  |
`--------------------------------------*/
yyerrlab:
  /* Make sure we have latest lookahead translation.  See comments at
     user semantic actions for why this is necessary.  */
  yytoken = yychar == YYEMPTY ? YYSYMBOL_YYEMPTY : YYTRANSLATE (yychar);
  /* If not already recovering from an error, report this error.  */
  if (!yyerrstatus)
    {
      ++yynerrs;
      yyerror (YY_("syntax error"));
    }

  if (yyerrstatus == 3)
    {
      /* If just tried and failed to reuse lookahead token after an
         error, discard it.  */

      if (yychar <= YYEOF)
        {
          /* Return failure if at end of input.  */
          if (yychar == YYEOF)
            YYABORT;
        }
      else
        {
          yydestruct ("Error: discarding",
                      yytoken, &yylval);
          yychar = YYEMPTY;
        }
    }

  /* Else will try to reuse lookahead token after shifting the error
     token.  */
  goto yyerrlab1;


/*---------------------------------------------------.
| yyerrorlab -- error raised explicitly by YYERROR.  |
`---------------------------------------------------*/
yyerrorlab:
  /* Pacify compilers when the user code never invokes YYERROR and the
     label yyerrorlab therefore never appears in user code.  */
  if (0)
    YYERROR;
  ++yynerrs;

  /* Do not reclaim the symbols of the rule whose action triggered
     this YYERROR.  */
  YYPOPSTACK (yylen);
  yylen = 0;
  YY_STACK_PRINT (yyss, yyssp);
  yystate = *yyssp;
  goto yyerrlab1;


/*-------------------------------------------------------------.
| yyerrlab1 -- common code for both syntax error and YYERROR.  |
`-------------------------------------------------------------*/
yyerrlab1:
  yyerrstatus = 3;      /* Each real token shifted decrements this.  */

  /* Pop stack until we find a state that shifts the error token.  */
  for (;;)
    {
      yyn = yypact[yystate];
      if (!yypact_value_is_default (yyn))
        {
          yyn += YYSYMBOL_YYerror;
          if (0 <= yyn && yyn <= YYLAST && yycheck[yyn] == YYSYMBOL_YYerror)
            {
              yyn = yytable[yyn];
              if (0 < yyn)
                break;
            }
        }

      /* Pop the current state because it cannot handle the error token.  */
      if (yyssp == yyss)
        YYABORT;


      yydestruct ("Error: popping",
                  YY_ACCESSING_SYMBOL (yystate), yyvsp);
      YYPOPSTACK (1);
      yystate = *yyssp;
      YY_STACK_PRINT (yyss, yyssp);
    }

  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  *++yyvsp = yylval;
  YY_IGNORE_MAYBE_UNINITIALIZED_END


  /* Shift the error token.  */
  YY_SYMBOL_PRINT ("Shifting", YY_ACCESSING_SYMBOL (yyn), yyvsp, yylsp);

  yystate = yyn;
  goto yynewstate;


/*-------------------------------------.
| yyacceptlab -- YYACCEPT comes here.  |
`-------------------------------------*/
yyacceptlab:
  yyresult = 0;
  goto yyreturnlab;


/*-----------------------------------.
| yyabortlab -- YYABORT comes here.  |
`-----------------------------------*/
yyabortlab:
  yyresult = 1;
  goto yyreturnlab;


/*-----------------------------------------------------------.
| yyexhaustedlab -- YYNOMEM (memory exhaustion) comes here.  |
`-----------------------------------------------------------*/
yyexhaustedlab:
  yyerror (YY_("memory exhausted"));
  yyresult = 2;
  goto yyreturnlab;


/*----------------------------------------------------------.
| yyreturnlab -- parsing is finished, clean up and return.  |
`----------------------------------------------------------*/
yyreturnlab:
  if (yychar != YYEMPTY)
    {
      /* Make sure we have latest lookahead translation.  See comments at
         user semantic actions for why this is necessary.  */
      yytoken = YYTRANSLATE (yychar);
      yydestruct ("Cleanup: discarding lookahead",
                  yytoken, &yylval);
    }
  /* Do not reclaim the symbols of the rule whose action triggered
     this YYABORT or YYACCEPT.  */
  YYPOPSTACK (yylen);
  YY_STACK_PRINT (yyss, yyssp);
  while (yyssp != yyss)
    {
      yydestruct ("Cleanup: popping",
                  YY_ACCESSING_SYMBOL (+*yyssp), yyvsp);
      YYPOPSTACK (1);
    }
#ifndef yyoverflow
  if (yyss != yyssa)
    YYSTACK_FREE (yyss);
#endif

  return yyresult;
}

