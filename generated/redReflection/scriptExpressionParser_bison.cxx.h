/* A Bison parser, made by GNU Bison 3.8.2.  */

/* Bison interface for Yacc-like parsers in C

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

/* DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
   especially those whose name start with YY_ or yy_.  They are
   private implementation details that can be changed or removed.  */

#ifndef YY_YY_D_CP2077_PORT_GENERATED_REDREFLECTION_SCRIPTEXPRESSIONPARSER_BISON_CXX_H_INCLUDED
# define YY_YY_D_CP2077_PORT_GENERATED_REDREFLECTION_SCRIPTEXPRESSIONPARSER_BISON_CXX_H_INCLUDED
/* Debug traces.  */
#ifndef YYDEBUG
# define YYDEBUG 0
#endif
#if YYDEBUG
extern int yydebug;
#endif

/* Token kinds.  */
#ifndef YYTOKENTYPE
# define YYTOKENTYPE
  enum yytokentype
  {
    YYEMPTY = -2,
    YYEOF = 0,                     /* "end of file"  */
    YYerror = 256,                 /* error  */
    YYUNDEF = 257,                 /* "invalid token"  */
    TOKEN_IDENT = 258,             /* TOKEN_IDENT  */
    TOKEN_INTEGER = 259,           /* TOKEN_INTEGER  */
    TOKEN_FLOAT = 260,             /* TOKEN_FLOAT  */
    TOKEN_NAME = 261,              /* TOKEN_NAME  */
    TOKEN_STRING = 262,            /* TOKEN_STRING  */
    TOKEN_TDBID = 263,             /* TOKEN_TDBID  */
    TOKEN_RESREF = 264,            /* TOKEN_RESREF  */
    TOKEN_BOOL_TRUE = 265,         /* TOKEN_BOOL_TRUE  */
    TOKEN_BOOL_FALSE = 266,        /* TOKEN_BOOL_FALSE  */
    TOKEN_NULL = 267,              /* TOKEN_NULL  */
    TOKEN_IMPORT = 268,            /* TOKEN_IMPORT  */
    TOKEN_IMPORTONLY = 269,        /* TOKEN_IMPORTONLY  */
    TOKEN_HOLY = 270,              /* TOKEN_HOLY  */
    TOKEN_ABSTRACT = 271,          /* TOKEN_ABSTRACT  */
    TOKEN_CONST = 272,             /* TOKEN_CONST  */
    TOKEN_MUTABLE = 273,           /* TOKEN_MUTABLE  */
    TOKEN_EXTENDS = 274,           /* TOKEN_EXTENDS  */
    TOKEN_CLASS = 275,             /* TOKEN_CLASS  */
    TOKEN_ENUM = 276,              /* TOKEN_ENUM  */
    TOKEN_STRUCT = 277,            /* TOKEN_STRUCT  */
    TOKEN_FUNCTION = 278,          /* TOKEN_FUNCTION  */
    TOKEN_DEF = 279,               /* TOKEN_DEF  */
    TOKEN_EDITABLE = 280,          /* TOKEN_EDITABLE  */
    TOKEN_INSTANCE_EDITABLE = 281, /* TOKEN_INSTANCE_EDITABLE  */
    TOKEN_REPLICATED = 282,        /* TOKEN_REPLICATED  */
    TOKEN_PERSISTENT = 283,        /* TOKEN_PERSISTENT  */
    TOKEN_FINAL = 284,             /* TOKEN_FINAL  */
    TOKEN_VIRTUAL = 285,           /* TOKEN_VIRTUAL  */
    TOKEN_OVERRIDE = 286,          /* TOKEN_OVERRIDE  */
    TOKEN_OUT = 287,               /* TOKEN_OUT  */
    TOKEN_OPTIONAL = 288,          /* TOKEN_OPTIONAL  */
    TOKEN_SKIP = 289,              /* TOKEN_SKIP  */
    TOKEN_INLINED = 290,           /* TOKEN_INLINED  */
    TOKEN_ARRAY = 291,             /* TOKEN_ARRAY  */
    TOKEN_HINT = 292,              /* TOKEN_HINT  */
    TOKEN_HINTS = 293,             /* TOKEN_HINTS  */
    TOKEN_DEFAULT = 294,           /* TOKEN_DEFAULT  */
    TOKEN_DEFAULTS = 295,          /* TOKEN_DEFAULTS  */
    TOKEN_BROWSABLE = 296,         /* TOKEN_BROWSABLE  */
    TOKEN_VAR = 297,               /* TOKEN_VAR  */
    TOKEN_EXEC = 298,              /* TOKEN_EXEC  */
    TOKEN_TIMER = 299,             /* TOKEN_TIMER  */
    TOKEN_WEAK = 300,              /* TOKEN_WEAK  */
    TOKEN_OPERATOR = 301,          /* TOKEN_OPERATOR  */
    TOKEN_CAST = 302,              /* TOKEN_CAST  */
    TOKEN_IMPLICIT = 303,          /* TOKEN_IMPLICIT  */
    TOKEN_STATIC = 304,            /* TOKEN_STATIC  */
    TOKEN_VISUAL = 305,            /* TOKEN_VISUAL  */
    TOKEN_CORE = 306,              /* TOKEN_CORE  */
    TOKEN_LOGIC = 307,             /* TOKEN_LOGIC  */
    TOKEN_MULTICAST = 308,         /* TOKEN_MULTICAST  */
    TOKEN_HOST = 309,              /* TOKEN_HOST  */
    TOKEN_CLIENT = 310,            /* TOKEN_CLIENT  */
    TOKEN_RELIABLE = 311,          /* TOKEN_RELIABLE  */
    TOKEN_QUEST = 312,             /* TOKEN_QUEST  */
    TOKEN_REF = 313,               /* TOKEN_REF  */
    TOKEN_DEBUG = 314,             /* TOKEN_DEBUG  */
    TOKEN_SYNCHRONIZED = 315,      /* TOKEN_SYNCHRONIZED  */
    TOKEN_SIMPLE_TYPE = 316,       /* TOKEN_SIMPLE_TYPE  */
    TOKEN_CLASS_TYPE = 317,        /* TOKEN_CLASS_TYPE  */
    TOKEN_ENUM_TYPE = 318,         /* TOKEN_ENUM_TYPE  */
    TOKEN_OP_IADD = 319,           /* TOKEN_OP_IADD  */
    TOKEN_OP_ISUB = 320,           /* TOKEN_OP_ISUB  */
    TOKEN_OP_IMUL = 321,           /* TOKEN_OP_IMUL  */
    TOKEN_OP_IDIV = 322,           /* TOKEN_OP_IDIV  */
    TOKEN_OP_IAND = 323,           /* TOKEN_OP_IAND  */
    TOKEN_OP_IOR = 324,            /* TOKEN_OP_IOR  */
    TOKEN_OP_LOGIC_OR = 325,       /* TOKEN_OP_LOGIC_OR  */
    TOKEN_OP_LOGIC_AND = 326,      /* TOKEN_OP_LOGIC_AND  */
    TOKEN_OP_EQUAL = 327,          /* TOKEN_OP_EQUAL  */
    TOKEN_OP_NOTEQUAL = 328,       /* TOKEN_OP_NOTEQUAL  */
    TOKEN_OP_GREQ = 329,           /* TOKEN_OP_GREQ  */
    TOKEN_OP_LEEQ = 330,           /* TOKEN_OP_LEEQ  */
    TOKEN_NEW = 331,               /* TOKEN_NEW  */
    TOKEN_IF = 332,                /* TOKEN_IF  */
    TOKEN_ELSE = 333,              /* TOKEN_ELSE  */
    TOKEN_SWITCH = 334,            /* TOKEN_SWITCH  */
    TOKEN_CASE = 335,              /* TOKEN_CASE  */
    TOKEN_FOR = 336,               /* TOKEN_FOR  */
    TOKEN_WHILE = 337,             /* TOKEN_WHILE  */
    TOKEN_DO = 338,                /* TOKEN_DO  */
    TOKEN_RETURN = 339,            /* TOKEN_RETURN  */
    TOKEN_BREAK = 340,             /* TOKEN_BREAK  */
    TOKEN_CONTINUE = 341,          /* TOKEN_CONTINUE  */
    TOKEN_THIS = 342,              /* TOKEN_THIS  */
    TOKEN_SUPER = 343,             /* TOKEN_SUPER  */
    TOKEN_PRIVATE = 344,           /* TOKEN_PRIVATE  */
    TOKEN_PROTECTED = 345,         /* TOKEN_PROTECTED  */
    TOKEN_PUBLIC = 346,            /* TOKEN_PUBLIC  */
    TOKEN_EVENT = 347,             /* TOKEN_EVENT  */
    TOKEN_TESTONLY = 348,          /* TOKEN_TESTONLY  */
    TOKEN_UNARY_LOGICAL_NOT = 349, /* TOKEN_UNARY_LOGICAL_NOT  */
    TOKEN_UNARY_BITWISE_NOT = 350  /* TOKEN_UNARY_BITWISE_NOT  */
  };
  typedef enum yytokentype yytoken_kind_t;
#endif

/* Value type.  */




int yyparse (ExpTokenStream* stream, script::expression::Path* path, script::expression::ErrorListener* errorListener);


#endif /* !YY_YY_D_CP2077_PORT_GENERATED_REDREFLECTION_SCRIPTEXPRESSIONPARSER_BISON_CXX_H_INCLUDED  */
