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
#define YYPURE 0

/* Push parsers.  */
#define YYPUSH 0

/* Pull parsers.  */
#define YYPULL 1




/* First part of user prologue.  */
#line 179 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"


#include "lexxer.h"
#include "strdefs.h"
#include "type.h"
#include "routine.h"
#include "statement.h"
#include "global.h"
#include "error.h"

static char *import_name();


#line 85 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"

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

#include "parser.h"
/* Symbol kind.  */
enum yysymbol_kind_t
{
  YYSYMBOL_YYEMPTY = -2,
  YYSYMBOL_YYEOF = 0,                      /* "end of file"  */
  YYSYMBOL_YYerror = 1,                    /* error  */
  YYSYMBOL_YYUNDEF = 2,                    /* "invalid token"  */
  YYSYMBOL_sySkip = 3,                     /* sySkip  */
  YYSYMBOL_syRoutine = 4,                  /* syRoutine  */
  YYSYMBOL_sySimpleRoutine = 5,            /* sySimpleRoutine  */
  YYSYMBOL_sySubsystem = 6,                /* sySubsystem  */
  YYSYMBOL_syKernelUser = 7,               /* syKernelUser  */
  YYSYMBOL_syKernelServer = 8,             /* syKernelServer  */
  YYSYMBOL_syMsgOption = 9,                /* syMsgOption  */
  YYSYMBOL_syMsgSeqno = 10,                /* syMsgSeqno  */
  YYSYMBOL_syWaitTime = 11,                /* syWaitTime  */
  YYSYMBOL_syNoWaitTime = 12,              /* syNoWaitTime  */
  YYSYMBOL_syErrorProc = 13,               /* syErrorProc  */
  YYSYMBOL_syServerPrefix = 14,            /* syServerPrefix  */
  YYSYMBOL_syUserPrefix = 15,              /* syUserPrefix  */
  YYSYMBOL_syServerDemux = 16,             /* syServerDemux  */
  YYSYMBOL_syRCSId = 17,                   /* syRCSId  */
  YYSYMBOL_syImport = 18,                  /* syImport  */
  YYSYMBOL_syUImport = 19,                 /* syUImport  */
  YYSYMBOL_sySImport = 20,                 /* sySImport  */
  YYSYMBOL_syIn = 21,                      /* syIn  */
  YYSYMBOL_syOut = 22,                     /* syOut  */
  YYSYMBOL_syInOut = 23,                   /* syInOut  */
  YYSYMBOL_syUserImpl = 24,                /* syUserImpl  */
  YYSYMBOL_syServerImpl = 25,              /* syServerImpl  */
  YYSYMBOL_syRequestPort = 26,             /* syRequestPort  */
  YYSYMBOL_syReplyPort = 27,               /* syReplyPort  */
  YYSYMBOL_sySReplyPort = 28,              /* sySReplyPort  */
  YYSYMBOL_syUReplyPort = 29,              /* syUReplyPort  */
  YYSYMBOL_syType = 30,                    /* syType  */
  YYSYMBOL_syArray = 31,                   /* syArray  */
  YYSYMBOL_syStruct = 32,                  /* syStruct  */
  YYSYMBOL_syOf = 33,                      /* syOf  */
  YYSYMBOL_syInTran = 34,                  /* syInTran  */
  YYSYMBOL_syOutTran = 35,                 /* syOutTran  */
  YYSYMBOL_syDestructor = 36,              /* syDestructor  */
  YYSYMBOL_syCType = 37,                   /* syCType  */
  YYSYMBOL_syCUserType = 38,               /* syCUserType  */
  YYSYMBOL_syCServerType = 39,             /* syCServerType  */
  YYSYMBOL_syCString = 40,                 /* syCString  */
  YYSYMBOL_syUserSecToken = 41,            /* syUserSecToken  */
  YYSYMBOL_syServerSecToken = 42,          /* syServerSecToken  */
  YYSYMBOL_syColon = 43,                   /* syColon  */
  YYSYMBOL_sySemi = 44,                    /* sySemi  */
  YYSYMBOL_syComma = 45,                   /* syComma  */
  YYSYMBOL_syPlus = 46,                    /* syPlus  */
  YYSYMBOL_syMinus = 47,                   /* syMinus  */
  YYSYMBOL_syStar = 48,                    /* syStar  */
  YYSYMBOL_syDiv = 49,                     /* syDiv  */
  YYSYMBOL_syLParen = 50,                  /* syLParen  */
  YYSYMBOL_syRParen = 51,                  /* syRParen  */
  YYSYMBOL_syEqual = 52,                   /* syEqual  */
  YYSYMBOL_syCaret = 53,                   /* syCaret  */
  YYSYMBOL_syTilde = 54,                   /* syTilde  */
  YYSYMBOL_syLAngle = 55,                  /* syLAngle  */
  YYSYMBOL_syRAngle = 56,                  /* syRAngle  */
  YYSYMBOL_syLBrack = 57,                  /* syLBrack  */
  YYSYMBOL_syRBrack = 58,                  /* syRBrack  */
  YYSYMBOL_syBar = 59,                     /* syBar  */
  YYSYMBOL_syError = 60,                   /* syError  */
  YYSYMBOL_syNumber = 61,                  /* syNumber  */
  YYSYMBOL_sySymbolicType = 62,            /* sySymbolicType  */
  YYSYMBOL_syIdentifier = 63,              /* syIdentifier  */
  YYSYMBOL_syString = 64,                  /* syString  */
  YYSYMBOL_syQString = 65,                 /* syQString  */
  YYSYMBOL_syFileName = 66,                /* syFileName  */
  YYSYMBOL_syIPCFlag = 67,                 /* syIPCFlag  */
  YYSYMBOL_YYACCEPT = 68,                  /* $accept  */
  YYSYMBOL_Statements = 69,                /* Statements  */
  YYSYMBOL_Statement = 70,                 /* Statement  */
  YYSYMBOL_Subsystem = 71,                 /* Subsystem  */
  YYSYMBOL_SubsystemStart = 72,            /* SubsystemStart  */
  YYSYMBOL_SubsystemMods = 73,             /* SubsystemMods  */
  YYSYMBOL_SubsystemMod = 74,              /* SubsystemMod  */
  YYSYMBOL_SubsystemName = 75,             /* SubsystemName  */
  YYSYMBOL_SubsystemBase = 76,             /* SubsystemBase  */
  YYSYMBOL_MsgOption = 77,                 /* MsgOption  */
  YYSYMBOL_WaitTime = 78,                  /* WaitTime  */
  YYSYMBOL_Error = 79,                     /* Error  */
  YYSYMBOL_ServerPrefix = 80,              /* ServerPrefix  */
  YYSYMBOL_UserPrefix = 81,                /* UserPrefix  */
  YYSYMBOL_ServerDemux = 82,               /* ServerDemux  */
  YYSYMBOL_Import = 83,                    /* Import  */
  YYSYMBOL_ImportIndicant = 84,            /* ImportIndicant  */
  YYSYMBOL_RCSDecl = 85,                   /* RCSDecl  */
  YYSYMBOL_TypeDecl = 86,                  /* TypeDecl  */
  YYSYMBOL_NamedTypeSpec = 87,             /* NamedTypeSpec  */
  YYSYMBOL_TransTypeSpec = 88,             /* TransTypeSpec  */
  YYSYMBOL_TypeSpec = 89,                  /* TypeSpec  */
  YYSYMBOL_BasicTypeSpec = 90,             /* BasicTypeSpec  */
  YYSYMBOL_PrimIPCType = 91,               /* PrimIPCType  */
  YYSYMBOL_IPCType = 92,                   /* IPCType  */
  YYSYMBOL_PrevTypeSpec = 93,              /* PrevTypeSpec  */
  YYSYMBOL_VarArrayHead = 94,              /* VarArrayHead  */
  YYSYMBOL_ArrayHead = 95,                 /* ArrayHead  */
  YYSYMBOL_StructHead = 96,                /* StructHead  */
  YYSYMBOL_CStringSpec = 97,               /* CStringSpec  */
  YYSYMBOL_IntExp = 98,                    /* IntExp  */
  YYSYMBOL_RoutineDecl = 99,               /* RoutineDecl  */
  YYSYMBOL_Routine = 100,                  /* Routine  */
  YYSYMBOL_SimpleRoutine = 101,            /* SimpleRoutine  */
  YYSYMBOL_Arguments = 102,                /* Arguments  */
  YYSYMBOL_ArgumentList = 103,             /* ArgumentList  */
  YYSYMBOL_Argument = 104,                 /* Argument  */
  YYSYMBOL_Trailer = 105,                  /* Trailer  */
  YYSYMBOL_Direction = 106,                /* Direction  */
  YYSYMBOL_TrImplKeyword = 107,            /* TrImplKeyword  */
  YYSYMBOL_TrExplKeyword = 108,            /* TrExplKeyword  */
  YYSYMBOL_ArgumentType = 109,             /* ArgumentType  */
  YYSYMBOL_IPCFlags = 110,                 /* IPCFlags  */
  YYSYMBOL_LookString = 111,               /* LookString  */
  YYSYMBOL_LookFileName = 112,             /* LookFileName  */
  YYSYMBOL_LookQString = 113               /* LookQString  */
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
typedef yytype_uint8 yy_state_t;

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
#define YYFINAL  2
/* YYLAST -- Last index in YYTABLE.  */
#define YYLAST   220

/* YYNTOKENS -- Number of terminals.  */
#define YYNTOKENS  68
/* YYNNTS -- Number of nonterminals.  */
#define YYNNTS  46
/* YYNRULES -- Number of rules.  */
#define YYNRULES  109
/* YYNSTATES -- Number of states.  */
#define YYNSTATES  213

/* YYMAXUTOK -- Last valid token kind.  */
#define YYMAXUTOK   322


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
      65,    66,    67
};

#if YYDEBUG
/* YYRLINE[YYN] -- Source line where rule number YYN was defined.  */
static const yytype_int16 yyrline[] =
{
       0,   216,   216,   217,   220,   221,   222,   223,   224,   225,
     226,   227,   228,   238,   240,   241,   242,   243,   247,   260,
     272,   273,   276,   287,   295,   298,   301,   318,   324,   332,
     340,   348,   356,   364,   375,   376,   377,   380,   390,   400,
     404,   406,   426,   446,   461,   475,   484,   496,   498,   500,
     502,   504,   506,   508,   512,   518,   525,   531,   535,   537,
     561,   565,   567,   569,   574,   578,   582,   584,   589,   591,
     593,   595,   597,   599,   604,   605,   608,   612,   616,   618,
     623,   625,   627,   632,   639,   649,   656,   667,   668,   669,
     670,   671,   672,   673,   674,   675,   676,   679,   680,   683,
     684,   685,   688,   694,   699,   700,   707,   716,   720,   724
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
  "\"end of file\"", "error", "\"invalid token\"", "sySkip", "syRoutine",
  "sySimpleRoutine", "sySubsystem", "syKernelUser", "syKernelServer",
  "syMsgOption", "syMsgSeqno", "syWaitTime", "syNoWaitTime", "syErrorProc",
  "syServerPrefix", "syUserPrefix", "syServerDemux", "syRCSId", "syImport",
  "syUImport", "sySImport", "syIn", "syOut", "syInOut", "syUserImpl",
  "syServerImpl", "syRequestPort", "syReplyPort", "sySReplyPort",
  "syUReplyPort", "syType", "syArray", "syStruct", "syOf", "syInTran",
  "syOutTran", "syDestructor", "syCType", "syCUserType", "syCServerType",
  "syCString", "syUserSecToken", "syServerSecToken", "syColon", "sySemi",
  "syComma", "syPlus", "syMinus", "syStar", "syDiv", "syLParen",
  "syRParen", "syEqual", "syCaret", "syTilde", "syLAngle", "syRAngle",
  "syLBrack", "syRBrack", "syBar", "syError", "syNumber", "sySymbolicType",
  "syIdentifier", "syString", "syQString", "syFileName", "syIPCFlag",
  "$accept", "Statements", "Statement", "Subsystem", "SubsystemStart",
  "SubsystemMods", "SubsystemMod", "SubsystemName", "SubsystemBase",
  "MsgOption", "WaitTime", "Error", "ServerPrefix", "UserPrefix",
  "ServerDemux", "Import", "ImportIndicant", "RCSDecl", "TypeDecl",
  "NamedTypeSpec", "TransTypeSpec", "TypeSpec", "BasicTypeSpec",
  "PrimIPCType", "IPCType", "PrevTypeSpec", "VarArrayHead", "ArrayHead",
  "StructHead", "CStringSpec", "IntExp", "RoutineDecl", "Routine",
  "SimpleRoutine", "Arguments", "ArgumentList", "Argument", "Trailer",
  "Direction", "TrImplKeyword", "TrExplKeyword", "ArgumentType",
  "IPCFlags", "LookString", "LookFileName", "LookQString", YY_NULLPTR
};

static const char *
yysymbol_name (yysymbol_kind_t yysymbol)
{
  return yytname[yysymbol];
}
#endif

#define YYPACT_NINF (-122)

#define yypact_value_is_default(Yyn) \
  ((Yyn) == YYPACT_NINF)

#define YYTABLE_NINF (-110)

#define yytable_value_is_error(Yyn) \
  0

/* YYPACT[STATE-NUM] -- Index in YYTABLE of the portion describing
   STATE-NUM.  */
static const yytype_int16 yypact[] =
{
    -122,    54,  -122,   -28,   -22,   -38,   -12,  -122,  -122,   -10,
       1,    22,    23,    36,  -122,  -122,    56,  -122,    61,    62,
      68,    74,    75,    76,    86,   101,   102,   104,  -122,  -122,
       2,    96,   123,  -122,  -122,    94,    94,  -122,  -122,  -122,
    -122,   110,  -122,  -122,    -7,  -122,  -122,  -122,  -122,  -122,
    -122,  -122,  -122,  -122,  -122,    92,    99,  -122,  -122,  -122,
      98,   103,    66,  -122,  -122,   -26,  -122,  -122,  -122,  -122,
     105,  -122,  -122,  -122,  -122,  -122,  -122,  -122,  -122,  -122,
    -122,  -122,  -122,  -122,  -122,  -122,  -122,  -122,  -122,  -122,
     114,   125,   126,   108,   109,   111,   116,   118,   119,   -17,
     -26,  -122,  -122,  -122,    44,  -122,  -122,   120,  -122,  -122,
     -26,   -26,   -26,  -122,  -122,  -122,  -122,   100,   100,   124,
     124,   124,   -41,   -35,   -40,   132,  -122,   135,   137,   138,
     139,   140,   141,   -17,  -122,  -122,  -122,  -122,  -122,   122,
    -122,   142,  -122,   -39,   -35,   153,  -122,   -15,    -8,   145,
      55,   -35,   127,   128,   129,   130,   131,   133,  -122,   110,
    -122,   144,   134,   -35,   162,   106,  -122,   -35,   -35,   -35,
     -35,   165,   166,   -35,  -122,   112,   143,   146,   150,  -122,
    -122,  -122,   136,  -122,    85,  -122,  -122,   -20,   -20,  -122,
    -122,  -122,  -122,    89,   -33,   151,   152,   147,   148,   171,
    -122,  -122,   149,   154,   156,   155,  -122,   157,   160,  -122,
    -122,  -122,  -122
};

/* YYDEFACT[STATE-NUM] -- Default reduction number in state STATE-NUM.
   Performed when YYTABLE does not specify something else to do.  Zero
   means the default is an error.  */
static const yytype_int8 yydefact[] =
{
       2,     0,     1,     0,     0,     0,     0,    19,    28,     0,
       0,     0,     0,     0,    16,     3,     0,    20,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,    74,    75,
       0,     0,     0,    17,    13,     0,     0,    29,    30,    31,
      32,     0,    38,     4,     0,     6,     5,     7,     8,     9,
      10,    14,    15,    11,    12,     0,     0,    34,    35,    36,
       0,     0,    87,    76,    77,     0,    22,    23,    24,    21,
       0,    26,    27,    33,    37,    96,   101,    95,    88,    89,
      90,    98,    97,    91,    92,    93,    94,   100,    99,    78,
       0,    80,    81,     0,     0,     0,     0,     0,     0,     0,
       0,    56,    57,    60,    39,    40,    47,    58,    54,    48,
       0,     0,     0,    53,    25,    18,    79,    87,    87,     0,
       0,     0,     0,     0,     0,     0,    51,     0,     0,     0,
       0,     0,     0,     0,    49,    50,    52,    82,    83,     0,
     104,     0,    85,     0,     0,     0,    72,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,    59,   102,
     103,    84,     0,     0,     0,     0,    61,     0,     0,     0,
       0,     0,     0,     0,    66,   104,     0,     0,     0,    44,
      45,    46,     0,    86,     0,    62,    73,    68,    69,    70,
      71,    64,    65,     0,     0,     0,     0,     0,   105,     0,
      67,    55,     0,     0,     0,     0,    63,     0,     0,    43,
     106,    41,    42
};

/* YYPGOTO[NTERM-NUM].  */
static const yytype_int16 yypgoto[] =
{
    -122,  -122,  -122,  -122,  -122,  -122,  -122,  -122,  -122,  -122,
    -122,  -122,  -122,  -122,  -122,  -122,  -122,  -122,  -122,    77,
    -122,    39,  -122,    81,   121,  -122,  -122,  -122,  -122,  -122,
    -121,  -122,  -122,  -122,   179,   -56,  -122,  -122,  -122,  -122,
    -122,   -24,    43,  -122,  -122,  -122
};

/* YYDEFGOTO[NTERM-NUM].  */
static const yytype_uint8 yydefgoto[] =
{
       0,     1,    15,    16,    17,    44,    69,    70,   115,    18,
      19,    20,    21,    22,    23,    24,    60,    25,    26,    42,
     104,   105,   106,   107,   108,   109,   110,   111,   112,   113,
     147,    27,    28,    29,    63,    90,    91,    92,    93,    94,
      95,   140,   161,    30,    31,    32
};

/* YYTABLE[YYPACT[STATE-NUM]] -- What to do in state STATE-NUM.  If
   positive, shift that token.  If negative, reduce the rule whose
   number is the opposite.  If YYTABLE_NINF, syntax error.  */
static const yytype_int16 yytable[] =
{
      66,    67,   148,   150,   163,    96,    97,   143,   149,   144,
     144,    55,   182,    56,    98,   144,    33,   145,   201,   164,
     146,   146,    34,   165,    99,    35,   146,   100,   169,   170,
     175,   167,   168,   169,   170,   101,   102,   103,   167,   168,
     169,   170,   184,   171,   101,   102,   187,   188,   189,   190,
     172,    36,   193,    37,     2,     3,    68,     4,     5,     6,
       7,   137,   138,  -107,    38,  -107,     8,     9,    10,    11,
      12,  -109,  -108,  -108,  -108,    75,    76,    77,   127,   128,
     129,   130,   131,   132,    13,    39,    40,    78,    79,    80,
      81,    82,    83,    84,    85,    86,   141,   142,    14,    41,
      43,   167,   168,   169,   170,    45,    46,    87,    88,    75,
      76,    77,    47,   174,    57,    58,    59,    89,    48,    49,
      50,    78,    79,    80,    81,    82,    83,    84,    85,    86,
      51,   167,   168,   169,   170,   167,   168,   169,   170,   126,
      61,    87,    88,   199,    62,    52,    53,   200,    54,   134,
     135,   136,   167,   168,   169,   170,    71,   186,   167,   168,
     169,   170,    65,    72,    73,   116,   114,   139,    74,   117,
     118,   119,   120,   122,   121,   123,   124,   151,   152,   133,
     153,   154,   155,   156,   157,   159,   166,   162,   173,   182,
     176,   177,   178,   179,   180,   185,   181,   183,   191,   192,
     197,   202,   203,   198,   206,   205,   195,   209,   211,   196,
     204,   212,   207,   210,   158,    64,   160,   208,   194,     0,
     125
};

static const yytype_int16 yycheck[] =
{
       7,     8,   123,   124,    43,    31,    32,    48,    48,    50,
      50,     9,    45,    11,    40,    50,    44,    58,    51,    58,
      61,    61,    44,   144,    50,    63,    61,    53,    48,    49,
     151,    46,    47,    48,    49,    61,    62,    63,    46,    47,
      48,    49,   163,    58,    61,    62,   167,   168,   169,   170,
      58,    63,   173,    63,     0,     1,    63,     3,     4,     5,
       6,   117,   118,     9,    63,    11,    12,    13,    14,    15,
      16,    17,    18,    19,    20,     9,    10,    11,    34,    35,
      36,    37,    38,    39,    30,    63,    63,    21,    22,    23,
      24,    25,    26,    27,    28,    29,   120,   121,    44,    63,
      44,    46,    47,    48,    49,    44,    44,    41,    42,     9,
      10,    11,    44,    58,    18,    19,    20,    51,    44,    44,
      44,    21,    22,    23,    24,    25,    26,    27,    28,    29,
      44,    46,    47,    48,    49,    46,    47,    48,    49,   100,
      17,    41,    42,    58,    50,    44,    44,    58,    44,   110,
     111,   112,    46,    47,    48,    49,    64,    51,    46,    47,
      48,    49,    52,    64,    66,    51,    61,    43,    65,    44,
      44,    63,    63,    57,    63,    57,    57,    45,    43,    59,
      43,    43,    43,    43,    43,    63,    33,    45,    43,    45,
      63,    63,    63,    63,    63,    33,    63,    63,    33,    33,
      50,    50,    50,    67,    33,    57,    63,    51,    51,    63,
      63,    51,    63,    58,   133,    36,   139,    63,   175,    -1,
      99
};

/* YYSTOS[STATE-NUM] -- The symbol kind of the accessing symbol of
   state STATE-NUM.  */
static const yytype_int8 yystos[] =
{
       0,    69,     0,     1,     3,     4,     5,     6,    12,    13,
      14,    15,    16,    30,    44,    70,    71,    72,    77,    78,
      79,    80,    81,    82,    83,    85,    86,    99,   100,   101,
     111,   112,   113,    44,    44,    63,    63,    63,    63,    63,
      63,    63,    87,    44,    73,    44,    44,    44,    44,    44,
      44,    44,    44,    44,    44,     9,    11,    18,    19,    20,
      84,    17,    50,   102,   102,    52,     7,     8,    63,    74,
      75,    64,    64,    66,    65,     9,    10,    11,    21,    22,
      23,    24,    25,    26,    27,    28,    29,    41,    42,    51,
     103,   104,   105,   106,   107,   108,    31,    32,    40,    50,
      53,    61,    62,    63,    88,    89,    90,    91,    92,    93,
      94,    95,    96,    97,    61,    76,    51,    44,    44,    63,
      63,    63,    57,    57,    57,    92,    89,    34,    35,    36,
      37,    38,    39,    59,    89,    89,    89,   103,   103,    43,
     109,   109,   109,    48,    50,    58,    61,    98,    98,    48,
      98,    45,    43,    43,    43,    43,    43,    43,    91,    63,
      87,   110,    45,    43,    58,    98,    33,    46,    47,    48,
      49,    58,    58,    43,    58,    98,    63,    63,    63,    63,
      63,    63,    45,    63,    98,    33,    51,    98,    98,    98,
      98,    33,    33,    98,   110,    63,    63,    50,    67,    58,
      58,    51,    50,    50,    63,    57,    33,    63,    63,    51,
      58,    51,    51
};

/* YYR1[RULE-NUM] -- Symbol kind of the left-hand side of rule RULE-NUM.  */
static const yytype_int8 yyr1[] =
{
       0,    68,    69,    69,    70,    70,    70,    70,    70,    70,
      70,    70,    70,    70,    70,    70,    70,    70,    71,    72,
      73,    73,    74,    74,    75,    76,    77,    78,    78,    79,
      80,    81,    82,    83,    84,    84,    84,    85,    86,    87,
      88,    88,    88,    88,    88,    88,    88,    89,    89,    89,
      89,    89,    89,    89,    90,    90,    91,    91,    92,    92,
      93,    94,    94,    94,    95,    96,    97,    97,    98,    98,
      98,    98,    98,    98,    99,    99,   100,   101,   102,   102,
     103,   103,   103,   103,   104,   105,   105,   106,   106,   106,
     106,   106,   106,   106,   106,   106,   106,   107,   107,   108,
     108,   108,   109,   109,   110,   110,   110,   111,   112,   113
};

/* YYR2[RULE-NUM] -- Number of symbols on the right-hand side of rule RULE-NUM.  */
static const yytype_int8 yyr2[] =
{
       0,     2,     0,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     1,     2,     4,     1,
       0,     2,     1,     1,     1,     1,     3,     3,     1,     2,
       2,     2,     2,     3,     1,     1,     1,     3,     2,     3,
       1,     8,     8,     7,     4,     4,     4,     1,     1,     2,
       2,     2,     2,     1,     1,     6,     1,     1,     1,     3,
       1,     4,     5,     7,     5,     5,     4,     6,     3,     3,
       3,     3,     1,     3,     1,     1,     3,     3,     2,     3,
       1,     1,     3,     3,     4,     3,     5,     0,     1,     1,
       1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
       1,     1,     2,     2,     0,     3,     5,     0,     0,     0
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


/* Lookahead token kind.  */
int yychar;

/* The semantic value of the lookahead symbol.  */
YYSTYPE yylval;
/* Number of syntax errors so far.  */
int yynerrs;




/*----------.
| yyparse.  |
`----------*/

int
yyparse (void)
{
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
      yychar = yylex ();
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
  case 12: /* Statement: RoutineDecl sySemi  */
#line 229 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
{
    register statement_t *st = stAlloc();

    st->stKind = skRoutine;
    st->stRoutine = (yyvsp[-1].routine);
    rtCheckRoutine((yyvsp[-1].routine));
    if (BeVerbose)
	rtPrintRoutine((yyvsp[-1].routine));
}
#line 1352 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 13: /* Statement: sySkip sySemi  */
#line 239 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                { rtSkip(); }
#line 1358 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 17: /* Statement: error sySemi  */
#line 244 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                { yyerrok; }
#line 1364 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 18: /* Subsystem: SubsystemStart SubsystemMods SubsystemName SubsystemBase  */
#line 249 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
{
    if (BeVerbose)
    {
	printf("Subsystem %s: base = %u%s%s\n\n",
	       SubsystemName, SubsystemBase,
	       IsKernelUser ? ", KernelUser" : "",
	       IsKernelServer ? ", KernelServer" : "");
    }
}
#line 1378 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 19: /* SubsystemStart: sySubsystem  */
#line 261 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
{
    if (SubsystemName != strNULL)
    {
	warn("previous Subsystem decl (of %s) will be ignored", SubsystemName);
	IsKernelUser = FALSE;
	IsKernelServer = FALSE;
	strfree(SubsystemName);
    }
}
#line 1392 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 22: /* SubsystemMod: syKernelUser  */
#line 277 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
{
    if (IsKernelUser)
	warn("duplicate KernelUser keyword");
    if (!UseMsgRPC) 
    {
	warn("with KernelUser the -R option is meaningless");
	UseMsgRPC = TRUE;
    }
    IsKernelUser = TRUE;
}
#line 1407 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 23: /* SubsystemMod: syKernelServer  */
#line 288 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
{
    if (IsKernelServer)
	warn("duplicate KernelServer keyword");
    IsKernelServer = TRUE;
}
#line 1417 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 24: /* SubsystemName: syIdentifier  */
#line 295 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                                { SubsystemName = (yyvsp[0].identifier); }
#line 1423 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 25: /* SubsystemBase: syNumber  */
#line 298 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                                { SubsystemBase = (yyvsp[0].number); }
#line 1429 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 26: /* MsgOption: LookString syMsgOption syString  */
#line 302 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
{
    if (streql((yyvsp[0].string), "MACH_MSG_OPTION_NONE"))
    {
	MsgOption = strNULL;
	if (BeVerbose)
	    printf("MsgOption: canceled\n\n");
    }
    else
    {
	MsgOption = (yyvsp[0].string);
	if (BeVerbose)
	    printf("MsgOption %s\n\n",(yyvsp[0].string));
    }
}
#line 1448 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 27: /* WaitTime: LookString syWaitTime syString  */
#line 319 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
{
    WaitTime = (yyvsp[0].string);
    if (BeVerbose)
	printf("WaitTime %s\n\n", WaitTime);
}
#line 1458 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 28: /* WaitTime: syNoWaitTime  */
#line 325 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
{
    WaitTime = strNULL;
    if (BeVerbose)
	printf("NoWaitTime\n\n");
}
#line 1468 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 29: /* Error: syErrorProc syIdentifier  */
#line 333 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
{
    ErrorProc = (yyvsp[0].identifier);
    if (BeVerbose)
	printf("ErrorProc %s\n\n", ErrorProc);
}
#line 1478 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 30: /* ServerPrefix: syServerPrefix syIdentifier  */
#line 341 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
{
    ServerPrefix = (yyvsp[0].identifier);
    if (BeVerbose)
	printf("ServerPrefix %s\n\n", ServerPrefix);
}
#line 1488 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 31: /* UserPrefix: syUserPrefix syIdentifier  */
#line 349 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
{
    UserPrefix = (yyvsp[0].identifier);
    if (BeVerbose)
	printf("UserPrefix %s\n\n", UserPrefix);
}
#line 1498 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 32: /* ServerDemux: syServerDemux syIdentifier  */
#line 357 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
{
    ServerDemux = (yyvsp[0].identifier);
    if (BeVerbose)
	printf("ServerDemux %s\n\n", ServerDemux);
}
#line 1508 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 33: /* Import: LookFileName ImportIndicant syFileName  */
#line 365 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
{
    register statement_t *st = stAlloc();
    st->stKind = (yyvsp[-1].statement_kind);
    st->stFileName = (yyvsp[0].string);

    if (BeVerbose)
	printf("%s %s\n\n", import_name((yyvsp[-1].statement_kind)), (yyvsp[0].string));
}
#line 1521 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 34: /* ImportIndicant: syImport  */
#line 375 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                                { (yyval.statement_kind) = skImport; }
#line 1527 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 35: /* ImportIndicant: syUImport  */
#line 376 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                                { (yyval.statement_kind) = skUImport; }
#line 1533 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 36: /* ImportIndicant: sySImport  */
#line 377 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                                { (yyval.statement_kind) = skSImport; }
#line 1539 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 37: /* RCSDecl: LookQString syRCSId syQString  */
#line 381 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
{
    if (RCSId != strNULL)
	warn("previous RCS decl will be ignored");
    if (BeVerbose)
	printf("RCSId %s\n\n", (yyvsp[0].string));
    RCSId = (yyvsp[0].string);
}
#line 1551 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 38: /* TypeDecl: syType NamedTypeSpec  */
#line 391 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
{
    register identifier_t name = (yyvsp[0].type)->itName;

    if (itLookUp(name) != itNULL)
	warn("overriding previous definition of %s", name);
    itInsert(name, (yyvsp[0].type));
}
#line 1563 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 39: /* NamedTypeSpec: syIdentifier syEqual TransTypeSpec  */
#line 401 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                { itTypeDecl((yyvsp[-2].identifier), (yyval.type) = (yyvsp[0].type)); }
#line 1569 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 40: /* TransTypeSpec: TypeSpec  */
#line 405 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                { (yyval.type) = itResetType((yyvsp[0].type)); }
#line 1575 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 41: /* TransTypeSpec: TransTypeSpec syInTran syColon syIdentifier syIdentifier syLParen syIdentifier syRParen  */
#line 408 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
{
    (yyval.type) = (yyvsp[-7].type);

    if (((yyval.type)->itTransType != strNULL) && !streql((yyval.type)->itTransType, (yyvsp[-4].identifier)))
	warn("conflicting translation types (%s, %s)",
	     (yyval.type)->itTransType, (yyvsp[-4].identifier));
    (yyval.type)->itTransType = (yyvsp[-4].identifier);

    if (((yyval.type)->itInTrans != strNULL) && !streql((yyval.type)->itInTrans, (yyvsp[-3].identifier)))
	warn("conflicting in-translation functions (%s, %s)",
	     (yyval.type)->itInTrans, (yyvsp[-3].identifier));
    (yyval.type)->itInTrans = (yyvsp[-3].identifier);

    if (((yyval.type)->itServerType != strNULL) && !streql((yyval.type)->itServerType, (yyvsp[-1].identifier)))
	warn("conflicting server types (%s, %s)",
	     (yyval.type)->itServerType, (yyvsp[-1].identifier));
    (yyval.type)->itServerType = (yyvsp[-1].identifier);
}
#line 1598 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 42: /* TransTypeSpec: TransTypeSpec syOutTran syColon syIdentifier syIdentifier syLParen syIdentifier syRParen  */
#line 428 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
{
    (yyval.type) = (yyvsp[-7].type);

    if (((yyval.type)->itServerType != strNULL) && !streql((yyval.type)->itServerType, (yyvsp[-4].identifier)))
	warn("conflicting server types (%s, %s)",
	     (yyval.type)->itServerType, (yyvsp[-4].identifier));
    (yyval.type)->itServerType = (yyvsp[-4].identifier);

    if (((yyval.type)->itOutTrans != strNULL) && !streql((yyval.type)->itOutTrans, (yyvsp[-3].identifier)))
	warn("conflicting out-translation functions (%s, %s)",
	     (yyval.type)->itOutTrans, (yyvsp[-3].identifier));
    (yyval.type)->itOutTrans = (yyvsp[-3].identifier);

    if (((yyval.type)->itTransType != strNULL) && !streql((yyval.type)->itTransType, (yyvsp[-1].identifier)))
	warn("conflicting translation types (%s, %s)",
	     (yyval.type)->itTransType, (yyvsp[-1].identifier));
    (yyval.type)->itTransType = (yyvsp[-1].identifier);
}
#line 1621 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 43: /* TransTypeSpec: TransTypeSpec syDestructor syColon syIdentifier syLParen syIdentifier syRParen  */
#line 448 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
{
    (yyval.type) = (yyvsp[-6].type);

    if (((yyval.type)->itDestructor != strNULL) && !streql((yyval.type)->itDestructor, (yyvsp[-3].identifier)))
	warn("conflicting destructor functions (%s, %s)",
	     (yyval.type)->itDestructor, (yyvsp[-3].identifier));
    (yyval.type)->itDestructor = (yyvsp[-3].identifier);

    if (((yyval.type)->itTransType != strNULL) && !streql((yyval.type)->itTransType, (yyvsp[-1].identifier)))
	warn("conflicting translation types (%s, %s)",
	     (yyval.type)->itTransType, (yyvsp[-1].identifier));
    (yyval.type)->itTransType = (yyvsp[-1].identifier);
}
#line 1639 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 44: /* TransTypeSpec: TransTypeSpec syCType syColon syIdentifier  */
#line 462 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
{
    (yyval.type) = (yyvsp[-3].type);

    if (((yyval.type)->itUserType != strNULL) && !streql((yyval.type)->itUserType, (yyvsp[0].identifier)))
	warn("conflicting user types (%s, %s)",
	     (yyval.type)->itUserType, (yyvsp[0].identifier));
    (yyval.type)->itUserType = (yyvsp[0].identifier);

    if (((yyval.type)->itServerType != strNULL) && !streql((yyval.type)->itServerType, (yyvsp[0].identifier)))
	warn("conflicting server types (%s, %s)",
	     (yyval.type)->itServerType, (yyvsp[0].identifier));
    (yyval.type)->itServerType = (yyvsp[0].identifier);
}
#line 1657 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 45: /* TransTypeSpec: TransTypeSpec syCUserType syColon syIdentifier  */
#line 476 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
{
    (yyval.type) = (yyvsp[-3].type);

    if (((yyval.type)->itUserType != strNULL) && !streql((yyval.type)->itUserType, (yyvsp[0].identifier)))
	warn("conflicting user types (%s, %s)",
	     (yyval.type)->itUserType, (yyvsp[0].identifier));
    (yyval.type)->itUserType = (yyvsp[0].identifier);
}
#line 1670 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 46: /* TransTypeSpec: TransTypeSpec syCServerType syColon syIdentifier  */
#line 486 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
{
    (yyval.type) = (yyvsp[-3].type);

    if (((yyval.type)->itServerType != strNULL) && !streql((yyval.type)->itServerType, (yyvsp[0].identifier)))
	warn("conflicting server types (%s, %s)",
	     (yyval.type)->itServerType, (yyvsp[0].identifier));
    (yyval.type)->itServerType = (yyvsp[0].identifier);
}
#line 1683 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 47: /* TypeSpec: BasicTypeSpec  */
#line 497 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                { (yyval.type) = (yyvsp[0].type); }
#line 1689 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 48: /* TypeSpec: PrevTypeSpec  */
#line 499 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                { (yyval.type) = (yyvsp[0].type); }
#line 1695 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 49: /* TypeSpec: VarArrayHead TypeSpec  */
#line 501 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                { (yyval.type) = itVarArrayDecl((yyvsp[-1].number), (yyvsp[0].type)); }
#line 1701 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 50: /* TypeSpec: ArrayHead TypeSpec  */
#line 503 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                { (yyval.type) = itArrayDecl((yyvsp[-1].number), (yyvsp[0].type)); }
#line 1707 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 51: /* TypeSpec: syCaret TypeSpec  */
#line 505 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                { (yyval.type) = itPtrDecl((yyvsp[0].type)); }
#line 1713 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 52: /* TypeSpec: StructHead TypeSpec  */
#line 507 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                { (yyval.type) = itStructDecl((yyvsp[-1].number), (yyvsp[0].type)); }
#line 1719 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 53: /* TypeSpec: CStringSpec  */
#line 509 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                { (yyval.type) = (yyvsp[0].type); }
#line 1725 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 54: /* BasicTypeSpec: IPCType  */
#line 513 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
{
    (yyval.type) = itShortDecl((yyvsp[0].symtype).innumber, (yyvsp[0].symtype).instr,
		     (yyvsp[0].symtype).outnumber, (yyvsp[0].symtype).outstr,
		     (yyvsp[0].symtype).size);
}
#line 1735 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 55: /* BasicTypeSpec: syLParen IPCType syComma IntExp IPCFlags syRParen  */
#line 520 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
{
    error("Long form type declarations aren't allowed anylonger\n");
}
#line 1743 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 56: /* PrimIPCType: syNumber  */
#line 526 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
{
    (yyval.symtype).innumber = (yyval.symtype).outnumber = (yyvsp[0].number);
    (yyval.symtype).instr = (yyval.symtype).outstr = strNULL;
    (yyval.symtype).size = 0;
}
#line 1753 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 57: /* PrimIPCType: sySymbolicType  */
#line 532 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                { (yyval.symtype) = (yyvsp[0].symtype); }
#line 1759 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 58: /* IPCType: PrimIPCType  */
#line 536 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                { (yyval.symtype) = (yyvsp[0].symtype); }
#line 1765 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 59: /* IPCType: PrimIPCType syBar PrimIPCType  */
#line 538 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
{
    if ((yyvsp[-2].symtype).size != (yyvsp[0].symtype).size)
    {
	if ((yyvsp[-2].symtype).size == 0)
	    (yyval.symtype).size = (yyvsp[0].symtype).size;
	else if ((yyvsp[0].symtype).size == 0)
	    (yyval.symtype).size = (yyvsp[-2].symtype).size;
	else
	{
	    error("sizes in IPCTypes (%d, %d) aren't equal",
		  (yyvsp[-2].symtype).size, (yyvsp[0].symtype).size);
	    (yyval.symtype).size = 0;
	}
    }
    else
	(yyval.symtype).size = (yyvsp[-2].symtype).size;
    (yyval.symtype).innumber = (yyvsp[-2].symtype).innumber;
    (yyval.symtype).instr = (yyvsp[-2].symtype).instr;
    (yyval.symtype).outnumber = (yyvsp[0].symtype).outnumber;
    (yyval.symtype).outstr = (yyvsp[0].symtype).outstr;
}
#line 1791 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 60: /* PrevTypeSpec: syIdentifier  */
#line 562 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                { (yyval.type) = itPrevDecl((yyvsp[0].identifier)); }
#line 1797 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 61: /* VarArrayHead: syArray syLBrack syRBrack syOf  */
#line 566 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                { (yyval.number) = 0; }
#line 1803 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 62: /* VarArrayHead: syArray syLBrack syStar syRBrack syOf  */
#line 568 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                { (yyval.number) = 0; }
#line 1809 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 63: /* VarArrayHead: syArray syLBrack syStar syColon IntExp syRBrack syOf  */
#line 571 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                { (yyval.number) = (yyvsp[-2].number); }
#line 1815 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 64: /* ArrayHead: syArray syLBrack IntExp syRBrack syOf  */
#line 575 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                { (yyval.number) = (yyvsp[-2].number); }
#line 1821 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 65: /* StructHead: syStruct syLBrack IntExp syRBrack syOf  */
#line 579 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                { (yyval.number) = (yyvsp[-2].number); }
#line 1827 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 66: /* CStringSpec: syCString syLBrack IntExp syRBrack  */
#line 583 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                { (yyval.type) = itCStringDecl((yyvsp[-1].number), FALSE); }
#line 1833 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 67: /* CStringSpec: syCString syLBrack syStar syColon IntExp syRBrack  */
#line 586 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                { (yyval.type) = itCStringDecl((yyvsp[-1].number), TRUE); }
#line 1839 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 68: /* IntExp: IntExp syPlus IntExp  */
#line 590 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                { (yyval.number) = (yyvsp[-2].number) + (yyvsp[0].number);	}
#line 1845 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 69: /* IntExp: IntExp syMinus IntExp  */
#line 592 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                { (yyval.number) = (yyvsp[-2].number) - (yyvsp[0].number);	}
#line 1851 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 70: /* IntExp: IntExp syStar IntExp  */
#line 594 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                { (yyval.number) = (yyvsp[-2].number) * (yyvsp[0].number);	}
#line 1857 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 71: /* IntExp: IntExp syDiv IntExp  */
#line 596 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                { (yyval.number) = (yyvsp[-2].number) / (yyvsp[0].number);	}
#line 1863 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 72: /* IntExp: syNumber  */
#line 598 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                { (yyval.number) = (yyvsp[0].number);	}
#line 1869 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 73: /* IntExp: syLParen IntExp syRParen  */
#line 600 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                { (yyval.number) = (yyvsp[-1].number);	}
#line 1875 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 74: /* RoutineDecl: Routine  */
#line 604 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                                        { (yyval.routine) = (yyvsp[0].routine); }
#line 1881 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 75: /* RoutineDecl: SimpleRoutine  */
#line 605 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                                        { (yyval.routine) = (yyvsp[0].routine); }
#line 1887 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 76: /* Routine: syRoutine syIdentifier Arguments  */
#line 609 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                { (yyval.routine) = rtMakeRoutine((yyvsp[-1].identifier), (yyvsp[0].argument)); }
#line 1893 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 77: /* SimpleRoutine: sySimpleRoutine syIdentifier Arguments  */
#line 613 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                { (yyval.routine) = rtMakeSimpleRoutine((yyvsp[-1].identifier), (yyvsp[0].argument)); }
#line 1899 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 78: /* Arguments: syLParen syRParen  */
#line 617 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                { (yyval.argument) = argNULL; }
#line 1905 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 79: /* Arguments: syLParen ArgumentList syRParen  */
#line 619 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                { (yyval.argument) = (yyvsp[-1].argument); }
#line 1911 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 80: /* ArgumentList: Argument  */
#line 624 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                { (yyval.argument) = (yyvsp[0].argument); }
#line 1917 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 81: /* ArgumentList: Trailer  */
#line 626 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                { (yyval.argument) = (yyvsp[0].argument); }
#line 1923 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 82: /* ArgumentList: Argument sySemi ArgumentList  */
#line 628 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
{
    (yyval.argument) = (yyvsp[-2].argument);
    (yyval.argument)->argNext = (yyvsp[0].argument);
}
#line 1932 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 83: /* ArgumentList: Trailer sySemi ArgumentList  */
#line 633 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
{
    (yyval.argument) = (yyvsp[-2].argument);
    (yyval.argument)->argNext = (yyvsp[0].argument);
}
#line 1941 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 84: /* Argument: Direction syIdentifier ArgumentType IPCFlags  */
#line 640 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
{
    (yyval.argument) = argAlloc();
    (yyval.argument)->argKind = (yyvsp[-3].direction);
    (yyval.argument)->argName = (yyvsp[-2].identifier);
    (yyval.argument)->argType = (yyvsp[-1].type);
    (yyval.argument)->argFlags = (yyvsp[0].flag);
}
#line 1953 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 85: /* Trailer: TrExplKeyword syIdentifier ArgumentType  */
#line 650 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
{
    (yyval.argument) = argAlloc();
    (yyval.argument)->argKind = (yyvsp[-2].direction);
    (yyval.argument)->argName = (yyvsp[-1].identifier);
    (yyval.argument)->argType = (yyvsp[0].type);
}
#line 1964 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 86: /* Trailer: TrImplKeyword syIdentifier ArgumentType syComma syIdentifier  */
#line 657 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
{
    (yyval.argument) = argAlloc();
    (yyval.argument)->argKind = (yyvsp[-4].direction);
    (yyval.argument)->argName = (yyvsp[-3].identifier);
    (yyval.argument)->argType = (yyvsp[-2].type);
    (yyval.argument)->argMsgField = (yyvsp[0].identifier);
}
#line 1976 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 87: /* Direction: %empty  */
#line 667 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                                { (yyval.direction) = akNone; }
#line 1982 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 88: /* Direction: syIn  */
#line 668 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                                { (yyval.direction) = akIn; }
#line 1988 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 89: /* Direction: syOut  */
#line 669 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                                { (yyval.direction) = akOut; }
#line 1994 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 90: /* Direction: syInOut  */
#line 670 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                                { (yyval.direction) = akInOut; }
#line 2000 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 91: /* Direction: syRequestPort  */
#line 671 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                                { (yyval.direction) = akRequestPort; }
#line 2006 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 92: /* Direction: syReplyPort  */
#line 672 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                                { (yyval.direction) = akReplyPort; }
#line 2012 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 93: /* Direction: sySReplyPort  */
#line 673 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                                { (yyval.direction) = akSReplyPort; }
#line 2018 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 94: /* Direction: syUReplyPort  */
#line 674 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                                { (yyval.direction) = akUReplyPort; }
#line 2024 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 95: /* Direction: syWaitTime  */
#line 675 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                                { (yyval.direction) = akWaitTime; }
#line 2030 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 96: /* Direction: syMsgOption  */
#line 676 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                                { (yyval.direction) = akMsgOption; }
#line 2036 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 97: /* TrImplKeyword: syServerImpl  */
#line 679 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                                { (yyval.direction) = akServerImpl; }
#line 2042 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 98: /* TrImplKeyword: syUserImpl  */
#line 680 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                                { (yyval.direction) = akUserImpl; }
#line 2048 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 99: /* TrExplKeyword: syServerSecToken  */
#line 683 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                                        { (yyval.direction) = akServerSecToken; }
#line 2054 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 100: /* TrExplKeyword: syUserSecToken  */
#line 684 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                                        { (yyval.direction) = akUserSecToken; }
#line 2060 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 101: /* TrExplKeyword: syMsgSeqno  */
#line 685 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                                { (yyval.direction) = akMsgSeqno; }
#line 2066 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 102: /* ArgumentType: syColon syIdentifier  */
#line 689 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
{
    (yyval.type) = itLookUp((yyvsp[0].identifier));
    if ((yyval.type) == itNULL)
	error("type '%s' not defined", (yyvsp[0].identifier));
}
#line 2076 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 103: /* ArgumentType: syColon NamedTypeSpec  */
#line 695 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                { (yyval.type) = (yyvsp[0].type); }
#line 2082 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 104: /* IPCFlags: %empty  */
#line 699 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                { (yyval.flag) = flNone; }
#line 2088 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 105: /* IPCFlags: IPCFlags syComma syIPCFlag  */
#line 701 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
{
    if ((yyvsp[-2].flag) & (yyvsp[0].flag))
	warn("redundant IPC flag ignored");
    else
	(yyval.flag) = (yyvsp[-2].flag) | (yyvsp[0].flag);
}
#line 2099 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 106: /* IPCFlags: IPCFlags syComma syIPCFlag syLBrack syRBrack  */
#line 708 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
{
    if ((yyvsp[-2].flag) != flDealloc)
	warn("only Dealloc is variable");
    else
	(yyval.flag) = (yyvsp[-4].flag) | flMaybeDealloc;
}
#line 2110 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 107: /* LookString: %empty  */
#line 716 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                { LookString(); }
#line 2116 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 108: /* LookFileName: %empty  */
#line 720 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                { LookFileName(); }
#line 2122 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;

  case 109: /* LookQString: %empty  */
#line 724 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"
                                { LookQString(); }
#line 2128 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"
    break;


#line 2132 "/home/slex/Scrivania/osfmk-mklinux/osfmk/build/src/mach_services/lib/migcom/parser.c"

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

#line 727 "/home/slex/Scrivania/osfmk-mklinux/osfmk/src/mach_services/lib/migcom/parser.y"


void
yyerror(s)
    char *s;
{
    error(s);
}

static char *
import_name(sk)
    statement_kind_t sk;
{
    switch (sk)
    {
      case skImport:
	return "Import";
      case skSImport:
	return "SImport";
      case skUImport:
	return "UImport";
      default:
	fatal("import_name(%d): not import statement", (int) sk);
	/*NOTREACHED*/
        return strNULL;
    }
}
