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

#ifndef YY_YY_HOME_SLEX_SCRIVANIA_OSFMK_MKLINUX_OSFMK_BUILD_SRC_MACH_SERVICES_LIB_MIGCOM_PARSER_H_INCLUDED
# define YY_YY_HOME_SLEX_SCRIVANIA_OSFMK_MKLINUX_OSFMK_BUILD_SRC_MACH_SERVICES_LIB_MIGCOM_PARSER_H_INCLUDED
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
    sySkip = 258,                  /* sySkip  */
    syRoutine = 259,               /* syRoutine  */
    sySimpleRoutine = 260,         /* sySimpleRoutine  */
    sySubsystem = 261,             /* sySubsystem  */
    syKernelUser = 262,            /* syKernelUser  */
    syKernelServer = 263,          /* syKernelServer  */
    syMsgOption = 264,             /* syMsgOption  */
    syMsgSeqno = 265,              /* syMsgSeqno  */
    syWaitTime = 266,              /* syWaitTime  */
    syNoWaitTime = 267,            /* syNoWaitTime  */
    syErrorProc = 268,             /* syErrorProc  */
    syServerPrefix = 269,          /* syServerPrefix  */
    syUserPrefix = 270,            /* syUserPrefix  */
    syServerDemux = 271,           /* syServerDemux  */
    syRCSId = 272,                 /* syRCSId  */
    syImport = 273,                /* syImport  */
    syUImport = 274,               /* syUImport  */
    sySImport = 275,               /* sySImport  */
    syIn = 276,                    /* syIn  */
    syOut = 277,                   /* syOut  */
    syInOut = 278,                 /* syInOut  */
    syUserImpl = 279,              /* syUserImpl  */
    syServerImpl = 280,            /* syServerImpl  */
    syRequestPort = 281,           /* syRequestPort  */
    syReplyPort = 282,             /* syReplyPort  */
    sySReplyPort = 283,            /* sySReplyPort  */
    syUReplyPort = 284,            /* syUReplyPort  */
    syType = 285,                  /* syType  */
    syArray = 286,                 /* syArray  */
    syStruct = 287,                /* syStruct  */
    syOf = 288,                    /* syOf  */
    syInTran = 289,                /* syInTran  */
    syOutTran = 290,               /* syOutTran  */
    syDestructor = 291,            /* syDestructor  */
    syCType = 292,                 /* syCType  */
    syCUserType = 293,             /* syCUserType  */
    syCServerType = 294,           /* syCServerType  */
    syCString = 295,               /* syCString  */
    syUserSecToken = 296,          /* syUserSecToken  */
    syServerSecToken = 297,        /* syServerSecToken  */
    syColon = 298,                 /* syColon  */
    sySemi = 299,                  /* sySemi  */
    syComma = 300,                 /* syComma  */
    syPlus = 301,                  /* syPlus  */
    syMinus = 302,                 /* syMinus  */
    syStar = 303,                  /* syStar  */
    syDiv = 304,                   /* syDiv  */
    syLParen = 305,                /* syLParen  */
    syRParen = 306,                /* syRParen  */
    syEqual = 307,                 /* syEqual  */
    syCaret = 308,                 /* syCaret  */
    syTilde = 309,                 /* syTilde  */
    syLAngle = 310,                /* syLAngle  */
    syRAngle = 311,                /* syRAngle  */
    syLBrack = 312,                /* syLBrack  */
    syRBrack = 313,                /* syRBrack  */
    syBar = 314,                   /* syBar  */
    syError = 315,                 /* syError  */
    syNumber = 316,                /* syNumber  */
    sySymbolicType = 317,          /* sySymbolicType  */
    syIdentifier = 318,            /* syIdentifier  */
    syString = 319,                /* syString  */
    syQString = 320,               /* syQString  */
    syFileName = 321,              /* syFileName  */
    syIPCFlag = 322                /* syIPCFlag  */
  };
  typedef enum yytokentype yytoken_kind_t;
#endif

/* Value type.  */
#if ! defined YYSTYPE && ! defined YYSTYPE_IS_DECLARED
union YYSTYPE
{
#line 194 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"

    u_int number;
    identifier_t identifier;
    string_t string;
    statement_kind_t statement_kind;
    ipc_type_t *type;
    struct
    {
	u_int innumber;		/* msgt_name value, when sending */
	string_t instr;
	u_int outnumber;	/* msgt_name value, when receiving */
	string_t outstr;
	u_int size;		/* 0 means there is no default size */
    } symtype;
    routine_t *routine;
    arg_kind_t direction;
    argument_t *argument;
    ipc_flags_t flag;

#line 151 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.h"

};
typedef union YYSTYPE YYSTYPE;
# define YYSTYPE_IS_TRIVIAL 1
# define YYSTYPE_IS_DECLARED 1
#endif


extern YYSTYPE yylval;


int yyparse (void);


#endif /* !YY_YY_HOME_SLEX_SCRIVANIA_OSFMK_MKLINUX_OSFMK_BUILD_SRC_MACH_SERVICES_LIB_MIGCOM_PARSER_H_INCLUDED  */
