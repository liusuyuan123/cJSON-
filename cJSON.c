/*
  Copyright (c) 2009-2017 Dave Gamble and cJSON contributors

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  THE SOFTWARE.
*/

/* cJSON */
/* JSON parser in C. */

/* disable warnings about old C89 functions in MSVC */
#if !defined(_CRT_SECURE_NO_DEPRECATE) && defined(_MSC_VER)
#define _CRT_SECURE_NO_DEPRECATE
#endif

#ifdef __GNUC__
#pragma GCC visibility push(default)
#endif
#if defined(_MSC_VER)
#pragma warning (push)
/* disable warning about single line comments in system headers */
#pragma warning (disable : 4001)
#endif

#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <limits.h>
#include <ctype.h>
#include <float.h>

#ifdef ENABLE_LOCALES
#include <locale.h>
#endif

#if defined(_MSC_VER)
#pragma warning (pop)
#endif
#ifdef __GNUC__
#pragma GCC visibility pop
#endif

#include "cJSON.h"

/* define our own boolean type */
#ifdef true
#undef true
#endif
#define true ((cJSON_bool)1)

#ifdef false
#undef false
#endif
#define false ((cJSON_bool)0)

/* define isnan and isinf for ANSI C, if in C99 or above, isnan and isinf has been defined in math.h */
#ifndef isinf
#define isinf(d) (isnan((d - d)) && !isnan(d))
#endif
#ifndef isnan
#define isnan(d) (d != d)
#endif

#ifndef NAN
#ifdef _WIN32
#define NAN sqrt(-1.0)
#else
#define NAN 0.0/0.0
#endif
#endif

typedef struct {
    const unsigned char *json;
    size_t position;
} error;
static error global_error = { NULL, 0 };

CJSON_PUBLIC(const char *) cJSON_GetErrorPtr(void)
{
    return (const char*) (global_error.json + global_error.position);
}

CJSON_PUBLIC(char *) cJSON_GetStringValue(const cJSON * const item)
{
    if (!cJSON_IsString(item))
    {
        return NULL;
    }

    return item->valuestring;
}

CJSON_PUBLIC(double) cJSON_GetNumberValue(const cJSON * const item)
{
    if (!cJSON_IsNumber(item))
    {
        return (double) NAN;
    }

    return item->valuedouble;
}

/* This is a safeguard to prevent copy-pasters from using incompatible C and header files */
#if (CJSON_VERSION_MAJOR != 1) || (CJSON_VERSION_MINOR != 7) || (CJSON_VERSION_PATCH != 19)
    #error cJSON.h and cJSON.c have different versions. Make sure that both have the same.
#endif

CJSON_PUBLIC(const char*) cJSON_Version(void)
{
    static char version[15];
    sprintf(version, "%i.%i.%i", CJSON_VERSION_MAJOR, CJSON_VERSION_MINOR, CJSON_VERSION_PATCH);

    return version;
}

/* Case insensitive string comparison, doesn't consider two NULL pointers equal though */
static int case_insensitive_strcmp(const unsigned char *string1, const unsigned char *string2)
{
    if ((string1 == NULL) || (string2 == NULL))
    {
        return 1;
    }

    if (string1 == string2)
    {
        return 0;
    }

    for(; tolower((int)*string1) == tolower((int)*string2); (void)string1++, string2++)
    {
        if (*string1 == '\0')
        {
            return 0;
        }
    }
    return tolower((int)*string1) - tolower((int)*string2);
    
}

typedef struct internal_hooks
{
    void *(CJSON_CDECL *allocate)(size_t size);
    void (CJSON_CDECL *deallocate)(void *pointer);
    void *(CJSON_CDECL *reallocate)(void *pointer, size_t size);
} internal_hooks;

#if defined(_MSC_VER)
/* work around MSVC error C2322: '...' address of dllimport '...' is not static */
static void * CJSON_CDECL internal_malloc(size_t size)
{
    return malloc(size);
}
static void CJSON_CDECL internal_free(void *pointer)
{
    free(pointer);
}
static void * CJSON_CDECL internal_realloc(void *pointer, size_t size)
{
    return realloc(pointer, size);
}
#else
#define internal_malloc malloc
#define internal_free free
#define internal_realloc realloc
#endif

/* strlen of character literals resolved at compile time */
#define static_strlen(string_literal) (sizeof(string_literal) - sizeof(""))

static internal_hooks global_hooks = { internal_malloc, internal_free, internal_realloc };

static unsigned char* cJSON_strdup(const unsigned char* string, const internal_hooks * const hooks)
{
    size_t length = 0;
    unsigned char *copy = NULL;

    if (string == NULL)
    {
        return NULL;
    }

    length = strlen((const char*)string) + 1;
    copy = (unsigned char*)hooks->allocate(length);
    if (copy == NULL)
    {
        return NULL;
    }
    memcpy(copy, string, length);

    return copy;
}
/*
 * 函数名：cJSON_InitHooks
 * 功能：cJSON内存钩子初始化公共函数 - 配置全局内存管理钩子（分配/释放/重分配）
 *       支持自定义内存分配/释放函数（适配嵌入式/内存池/自定义内存管理器），兼容系统默认malloc/free/realloc，保证内存管理的灵活性
 * 核心设计思路：
 *       1. 兼容优先：未传入钩子/钩子函数为NULL时，默认使用系统标准malloc/free/realloc；
 *       2. 安全约束：仅当分配/释放函数均为系统默认时，才启用realloc（避免自定义内存管理器与realloc不兼容）；
 *       3. 全局生效：修改全局global_hooks，所有cJSON内存操作（创建节点/扩容缓冲区等）均使用该钩子；
 *       4. 重置逻辑：传入NULL时，强制恢复为系统默认内存函数，便于重置状态。
 *
 * 参数：
 *   hooks  - cJSON_Hooks*，内存钩子配置结构体（包含自定义malloc_fn/free_fn），传NULL时重置为系统默认
 *
 * 返回值：无
 *
 * 关键术语：
 *   - cJSON_Hooks：cJSON内存钩子结构体，定义了malloc_fn（自定义分配）、free_fn（自定义释放）；
 *   - global_hooks：cJSON全局内存钩子，所有内存操作（如cJSON_New_Item/ensure扩容）均依赖此钩子；
 *   - CJSON_PUBLIC：宏定义，标记函数为公共API（对外暴露，可被用户调用）。
 *
 * cJSON_Hooks结构体定义（补充说明）：
 * typedef struct {
 *     void *(*malloc_fn)(size_t sz);  // 自定义内存分配函数（替代malloc）
 *     void (*free_fn)(void *ptr);     // 自定义内存释放函数（替代free）
 * } cJSON_Hooks;
 *
 * global_hooks结构体定义（补充说明）：
 * typedef struct {
 *     void *(*allocate)(size_t sz);   // 实际使用的分配函数
 *     void (*deallocate)(void *ptr);  // 实际使用的释放函数
 *     void *(*reallocate)(void *ptr, size_t sz); // 实际使用的重分配函数
 * } cJSON_MemoryHooks;
 * static cJSON_MemoryHooks global_hooks; // 全局内存钩子
 */
CJSON_PUBLIC(void) cJSON_InitHooks(cJSON_Hooks* hooks)
{
    /* 第一步：钩子重置逻辑 - 传入NULL时，强制恢复为系统默认内存函数
     * 场景：用户之前配置了自定义钩子，需要重置为默认行为
     */
    if (hooks == NULL)
    {
        global_hooks.allocate = internal_malloc;    // 恢复分配函数为系统malloc
        global_hooks.deallocate = internal_free;    // 恢复释放函数为系统free
        global_hooks.reallocate = internal_realloc; // 恢复重分配函数为系统realloc
        return;
    }

    /* 第二步：初始化分配函数 - 默认使用系统malloc，若自定义malloc_fn非NULL则替换 */
    global_hooks.allocate = malloc;
    if (hooks->malloc_fn != NULL)
    {
        global_hooks.allocate = hooks->malloc_fn;
    }

    /* 第三步：初始化释放函数 - 默认使用系统free，若自定义free_fn非NULL则替换 */
    global_hooks.deallocate = free;
    if (hooks->free_fn != NULL)
    {
        global_hooks.deallocate = hooks->free_fn;
    }

    /* 第四步：初始化重分配函数（核心安全约束）
     * 设计原则：仅当分配/释放函数均为系统默认时，才启用realloc
     * 原因：自定义内存管理器（如内存池）通常不兼容realloc（realloc可能跨池分配/释放），强制禁用避免内存错误
     */
    global_hooks.reallocate = NULL; // 先默认禁用realloc
    if ((global_hooks.allocate == malloc) && (global_hooks.deallocate == free))
    {
        global_hooks.reallocate = realloc; // 仅系统默认时启用realloc
    }
}
/* Internal constructor. */
/*
 * 函数名：cJSON_New_Item
 * 功能：cJSON库的底层节点创建函数，为单个cJSON节点分配内存并初始化默认值
 *       所有创建JSON节点的接口（如cJSON_CreateObject/cJSON_CreateString）均依赖此函数
 * 核心步骤：
 *   1. 内存分配：通过传入的内部内存钩子（internal_hooks）分配cJSON节点内存；
 *   2. 初始化：若内存分配成功，将节点所有字段置为'\0'（等价于0/NULL）；
 *   3. 返回值：成功则返回初始化后的空节点，失败则返回NULL。
 * 参数：
 *   hooks - const internal_hooks* const，内部内存钩子结构体指针（不可为NULL）
 *           包含自定义的内存分配/释放函数，用于节点的内存管理
 * 返回值：
 *   成功 - cJSON*，指向空cJSON节点的指针；失败 - NULL（内存分配不足/钩子非法）
 * 注意事项：
 *   1. 内存管理：创建的节点必须通过cJSON_Delete释放，否则会造成内存泄漏；
 *   2. 初始化规则：节点所有字段默认值为'\0'，需后续手动设置类型（type）、值（value）等；
 *   3. 钩子依赖：节点释放时会使用与该hooks匹配的释放函数，需保证钩子有效性；
 *   4. 空指针风险：若hooks->allocate返回NULL（内存分配失败），函数直接返回NULL，无崩溃风险。
 */
static cJSON *cJSON_New_Item(const internal_hooks * const hooks)
{
    //通过内部内存钩子分配cJSON节点内存（核心：自定义内存管理）
    cJSON* node = (cJSON*)hooks->allocate(sizeof(cJSON));
    if (node)
    {
        //初始化节点所有字段为'\0'，避免野值导致的未定义行为
        memset(node, '\0', sizeof(cJSON));
    }

    return node;
}
/*
 * 函数名：cJSON_Delete
 * 功能：递归释放cJSON节点及其子节点的内存（核心内存释放函数）
 *       支持区分引用节点/常量字符串，避免重复释放或误释放，是内存安全的关键
 * 核心步骤：
 *   1. 遍历节点链表：通过next指针遍历当前节点的所有同级节点；
 *   2. 递归释放子节点：仅非引用节点（!cJSON_IsReference）才释放child子树；
 *   3. 释放字符串值：仅非引用节点且valuestring非空时，释放并置空valuestring；
 *   4. 释放键名字符串：仅非常量字符串（!cJSON_StringIsConst）且string非空时，释放并置空string；
 *   5. 释放当前节点：通过全局内存钩子释放节点本身；
 *   6. 迭代处理：切换到下一个同级节点，重复步骤1-5直至所有节点释放完成。
 * 参数：
 *   item - cJSON*，待释放的cJSON节点（可为NULL，NULL时直接退出循环，无操作）
 * 返回值：无（void类型）
 * 注意事项：
 *   1. 空指针保护：传入NULL节点不会崩溃，函数直接退出；
 *   2. 引用节点保护：标记为cJSON_IsReference的节点，不释放子节点/valuestring，避免重复释放；
 *   3. 常量字符串保护：标记为cJSON_StringIsConst的节点，不释放键名string，避免释放只读内存；
 *   4. 置空指针：释放字符串后主动置NULL，避免野指针访问；
 *   5. 递归释放：只需调用根节点的Delete，会自动递归释放所有非引用子节点，无需逐个释放。
 */
/* Delete a cJSON structure. */
CJSON_PUBLIC(void) cJSON_Delete(cJSON *item)
{
    cJSON *next = NULL;
    //通过next指针遍历当前节点的所有同级节点
    while (item != NULL)
    {
        next = item->next;//先保存下一个节点指针，避免当前节点释放后丢失
        //step1:递归释放子节点，仅非引用节点才释放,避免重复释放
        if (!(item->type & cJSON_IsReference) && (item->child != NULL))
        {
            cJSON_Delete(item->child);
        }
        //step2:释放字符串值，仅非引用节点且valuestring非空时释放，避免误释放
        if (!(item->type & cJSON_IsReference) && (item->valuestring != NULL))
        {
            global_hooks.deallocate(item->valuestring);
            item->valuestring = NULL;//释放后置NULL，避免野指针访问
        }
        //step3:释放键名字符串，仅非常量字符串且string非空时释放，避免释放只读内存
        if (!(item->type & cJSON_StringIsConst) && (item->string != NULL))
        {
            global_hooks.deallocate(item->string);
            item->string = NULL;//释放后置NULL，避免野指针访问
        }
        //step4:释放当前节点，通过全局内存钩子释放节点本身
        global_hooks.deallocate(item);
        //step5:切换到下一个同级节点，重复步骤1-4直至所有节点释放完成
        item = next;
    }
}

/* get the decimal point character of the current locale */
static unsigned char get_decimal_point(void)
{
#ifdef ENABLE_LOCALES
    struct lconv *lconv = localeconv();
    return (unsigned char)(lconv ? lconv->decimal_point[0] : '.');
#else
    return '.';
#endif
}

typedef struct
{
    const unsigned char *content;
    size_t length;
    size_t offset;
    size_t depth; /* How deeply nested (in arrays/objects) is the input at the current offset. */
    internal_hooks hooks;
} parse_buffer;

/* check if the given size is left to read in a given parse buffer (starting with 1) */
#define can_read(buffer, size) ((buffer != NULL) && (((buffer)->offset + size) <= (buffer)->length))
/* check if the buffer can be accessed at the given index (starting with 0) */
#define can_access_at_index(buffer, index) ((buffer != NULL) && (((buffer)->offset + index) < (buffer)->length))
#define cannot_access_at_index(buffer, index) (!can_access_at_index(buffer, index))
/* get a pointer to the buffer at the position */
#define buffer_at_offset(buffer) ((buffer)->content + (buffer)->offset)

/* Parse the input text to generate a number, and populate the result into item. */
/*
 * 函数名：parse_number
 * 功能：JSON数值解析核心函数（私有静态）- 解析JSON数值（整数/小数/科学计数法）
 *       兼容JSON标准所有数值格式，适配系统本地化小数点（如欧洲用,替代.），处理数值溢出防护，最终填充到cJSON节点
 * 核心设计思路：
 *       1. 两步解析法：先提取数值字符串 → 本地化适配 → 调用strtod转换为浮点数，保证跨系统兼容性；
 *       2. 本地化兼容：替换JSON标准的.为系统当前locale的小数点（解决strtod解析失败问题）；
 *       3. 溢出防护：数值超出int范围时饱和处理（设为INT_MAX/INT_MIN），避免int溢出；
 *       4. 内存安全：临时缓冲区统一分配/释放，异常时无内存泄漏。
 *
 * 参数：
 *   item          - cJSON* const，待填充的cJSON节点（解析结果写入type/valuedouble/valueint字段）
 *   input_buffer  - parse_buffer* const，JSON解析缓冲区（包含待解析内容、偏移、内存钩子等）
 *
 * 返回值：
 *   cJSON_True(1) - 解析成功（数值合法，节点填充完成）；
 *   cJSON_False(0) - 解析失败（格式错误/内存不足/转换失败/参数非法）。
 *
 * 关键术语：
 *   - strtod：C标准库函数，将字符串转为double，支持科学计数法（如1e3=1000）；
 *   - locale：系统本地化设置，不同地区小数点符号不同（如en_US用.，fr_FR用,）；
 *   - 饱和处理：数值超出int范围时，固定为INT_MAX/INT_MIN，而非溢出为负数。
 */
static cJSON_bool parse_number(cJSON * const item, parse_buffer * const input_buffer)
{
    double number = 0;                  // 存储解析后的浮点数值
    unsigned char *after_end = NULL;    // strtod转换后，指向数值字符串末尾的指针（用于校验转换是否成功）
    unsigned char *number_c_string;     // 临时缓冲区：存储提取的数值字符串（本地化适配后）
    // 获取系统当前locale的小数点符号（如.或,），解决strtod跨系统解析问题
    unsigned char decimal_point = get_decimal_point();
    size_t i = 0;                       // 遍历索引
    size_t number_string_length = 0;    // 数值字符串长度（提取的合法字符数）
    cJSON_bool has_decimal_point = false; // 是否包含小数点（标记是否需要本地化替换）

    // 第一步：参数合法性校验（缓冲区/内容为NULL，直接返回失败）
    if ((input_buffer == NULL) || (input_buffer->content == NULL))
    {
        return false;
    }

    /* 第二步：提取数值字符串（从缓冲区中截取合法的数值字符）
     * 核心逻辑：遍历缓冲区，仅保留JSON数值合法字符（0-9/+/-/e/E/.），遇到非法字符停止遍历
     * 解决问题：缓冲区无\0终止符时，能准确截取数值范围，避免strtod解析越界
     */
    for (i = 0; can_access_at_index(input_buffer, i); i++)
    {
        switch (buffer_at_offset(input_buffer)[i])
        {
            // 合法字符：数字0-9
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9':
            // 合法字符：正负号（+/-）、科学计数法符号（e/E）
            case '+':
            case '-':
            case 'e':
            case 'E':
                number_string_length++; // 统计合法字符长度
                break;

            // 合法字符：小数点（JSON标准用.，后续需替换为系统locale的小数点）
            case '.':
                number_string_length++;
                has_decimal_point = true; // 标记需要本地化替换
                break;

            // 非法字符：停止遍历（如JSON的,/:/[/{等分隔符）
            default:
                goto loop_end;
        }
    }
loop_end: // 遍历终止标签（goto统一出口，避免多层break）

    /* 第三步：分配临时缓冲区（存储提取的数值字符串）
     * +1 为字符串结束符\0预留空间，符合C字符串规范
     */
    number_c_string = (unsigned char *) input_buffer->hooks.allocate(number_string_length + 1);
    if (number_c_string == NULL)
    {
        return false; /* allocation failure - 内存分配失败 */
    }

    // 拷贝提取的数值字符到临时缓冲区
    memcpy(number_c_string, buffer_at_offset(input_buffer), number_string_length);
    number_c_string[number_string_length] = '\0'; // 添加字符串结束符

    /* 第四步：本地化适配（替换小数点）
     * 核心目的：strtod依赖系统locale解析小数点，JSON标准用.，若系统locale用,（如法语区），直接解析会失败
     * 处理逻辑：仅当数值包含.时，替换为系统当前的decimal_point
     */
    if (has_decimal_point)
    {
        for (i = 0; i < number_string_length; i++)
        {
            if (number_c_string[i] == '.')
            {
                number_c_string[i] = decimal_point; // 替换为系统本地化小数点
            }
        }
    }
    /* 第五步：字符串转浮点数（调用strtod）
     * strtod参数说明：
     *   - 第一个参数：待转换的字符串（本地化适配后）；
     *   - 第二个参数：输出参数，指向转换后字符串末尾的指针（校验是否转换成功）
     */
    number = strtod((const char*)number_c_string, (char**)&after_end);

    /* 第六步：校验转换结果（避免非法数值） */
    // 转换失败（无有效数值） 或 数值为NaN/Inf（JSON不支持），返回失败
    if ((number_c_string == after_end) || isnan(number) || isinf(number))
    {
        input_buffer->hooks.deallocate(number_c_string); // 释放临时缓冲区
        return false;
    }

    /* 第七步：填充cJSON节点（数值结果写入节点） */
    item->valuedouble = number; // 存储浮点数值
    // 饱和处理：数值超出int范围时，设为INT_MAX/INT_MIN，避免int溢出
    if (number > (double)INT_MAX)
    {
        item->valueint = INT_MAX;
    }
    else if (number < (double)INT_MIN)
    {
        item->valueint = INT_MIN;
    }
    else
    {
        item->valueint = (int)number; // 数值在int范围内，直接强转
    }
    item->type = cJSON_Number; // 标记节点类型为数值

    /* 第八步：更新缓冲区偏移（跳过已解析的数值字符） */
    input_buffer->offset += (size_t)(after_end - number_c_string);

    /* 第九步：释放临时缓冲区（内存安全） */
    input_buffer->hooks.deallocate(number_c_string);

    return true; // 解析成功
}
/* don't ask me, but the original cJSON_SetNumberValue returns an integer or double */
CJSON_PUBLIC(double) cJSON_SetNumberHelper(cJSON *object, double number)
{
    if (number >= INT_MAX)
    {
        object->valueint = INT_MAX;
    }
    else if (number <= (double)INT_MIN)
    {
        object->valueint = INT_MIN;
    }
    else
    {
        object->valueint = (int)number;
    }

    return object->valuedouble = number;
}

/* Note: when passing a NULL valuestring, cJSON_SetValuestring treats this as an error and return NULL */
CJSON_PUBLIC(char*) cJSON_SetValuestring(cJSON *object, const char *valuestring)
{
    char *copy = NULL;
    size_t v1_len;
    size_t v2_len;
    /* if object's type is not cJSON_String or is cJSON_IsReference, it should not set valuestring */
    if ((object == NULL) || !(object->type & cJSON_String) || (object->type & cJSON_IsReference))
    {
        return NULL;
    }
    /* return NULL if the object is corrupted or valuestring is NULL */
    if (object->valuestring == NULL || valuestring == NULL)
    {
        return NULL;
    }

    v1_len = strlen(valuestring);
    v2_len = strlen(object->valuestring);

    if (v1_len <= v2_len)
    {
        /* strcpy does not handle overlapping string: [X1, X2] [Y1, Y2] => X2 < Y1 or Y2 < X1 */
        if (!( valuestring + v1_len < object->valuestring || object->valuestring + v2_len < valuestring ))
        {
            return NULL;
        }
        strcpy(object->valuestring, valuestring);
        return object->valuestring;
    }
    copy = (char*) cJSON_strdup((const unsigned char*)valuestring, &global_hooks);
    if (copy == NULL)
    {
        return NULL;
    }
    if (object->valuestring != NULL)
    {
        cJSON_free(object->valuestring);
    }
    object->valuestring = copy;

    return copy;
}

typedef struct
{
    unsigned char *buffer;
    size_t length;
    size_t offset;
    size_t depth; /* current nesting depth (for formatted printing) */
    cJSON_bool noalloc;
    cJSON_bool format; /* is this print a formatted print */
    internal_hooks hooks;
} printbuffer;

/* realloc printbuffer if necessary to have at least "needed" bytes more */
/*
 * 函数名：ensure
 * 功能：cJSON序列化缓冲区扩容核心函数（私有静态）- 保证打印缓冲区有足够空间存储待写入数据
 *       按需扩容缓冲区（2倍扩容/INT_MAX上限），支持自定义内存重分配钩子，全程校验内存溢出/越界，保证内存安全
 * 核心设计思路：
 *       1. 预校验优先：先校验参数/偏移/所需空间合法性，避免无效扩容操作；
 *       2. 按需扩容：仅当所需空间不足时才扩容，减少内存分配次数；
 *       3. 2倍扩容策略：常规场景下缓冲区大小翻倍，平衡内存利用率与扩容次数；
 *       4. 溢出防护：限制最大扩容尺寸为INT_MAX，避免size_t溢出为负数；
 *       5. 钩子适配：兼容自定义reallocate钩子（高效扩容）和默认allocate+memcpy（通用扩容）；
 *       6. 失败回滚：扩容失败时释放原缓冲区，避免内存泄漏。
 *
 * 参数：
 *   p       - printbuffer* const，打印缓冲区结构体（包含缓冲区指针、当前偏移、总长度、内存钩子等）
 *   needed  - size_t，本次需要写入的数据长度（字节数）
 *
 * 返回值：
 *   unsigned char* - 成功：指向缓冲区当前偏移位置的可写入指针；失败：NULL（参数非法/空间不足/扩容失败）。
 *
 * 关键术语：
 *   - printbuffer：cJSON序列化专用缓冲区，包含buffer（数据指针）、offset（当前写入偏移）、length（总长度）、hooks（内存钩子）；
 *   - 内存钩子：reallocate（高效重分配）/allocate（分配）/deallocate（释放），支持自定义内存管理；
 *   - INT_MAX：int类型最大值（2147483647），作为缓冲区最大长度上限，防止溢出。
 */
static unsigned char* ensure(printbuffer * const p, size_t needed)
{
    unsigned char *newbuffer = NULL; // 扩容后的新缓冲区指针
    size_t newsize = 0;              // 新缓冲区的总长度

    /* 第一步：参数合法性校验 - 缓冲区/缓冲区指针为NULL，直接返回失败 */
    if ((p == NULL) || (p->buffer == NULL))
    {
        return NULL;
    }

    /* 第二步：偏移合法性校验 - 当前偏移超出缓冲区长度（无效偏移），返回失败
     * 注：p->length>0 排除空缓冲区场景（空缓冲区offset=0，length=0，无需校验）
     */
    if ((p->length > 0) && (p->offset >= p->length))
    {
        return NULL; /* make sure that offset is valid - 偏移无效 */
    }

    /* 第三步：所需空间溢出校验 - needed超过INT_MAX，当前不支持超大空间分配 */
    if (needed > INT_MAX)
    {
        return NULL; /* sizes bigger than INT_MAX are currently not supported - 超出最大支持尺寸 */
    }

    /* 第四步：计算总所需空间 - 已用空间（offset） + 本次所需空间（needed） + 1（预留\0终止符）
     * +1 是为了保证序列化后的字符串以\0结尾，符合C语言字符串规范
     */
    needed += p->offset + 1;
    /* 空间足够：无需扩容，直接返回当前偏移的可写入指针 */
    if (needed <= p->length)
    {
        return p->buffer + p->offset;
    }

    /* 第五步：只读模式校验 - noalloc为true时禁止扩容（如静态缓冲区场景），返回失败 */
    if (p->noalloc) {
        return NULL;
    }

    /* 第六步：计算新缓冲区长度（扩容策略） */
    if (needed > (INT_MAX / 2))
    {
        /* 场景1：needed超过INT_MAX的一半 → 2倍扩容会溢出，改用INT_MAX作为上限
         * 溢出防护：避免newsize = needed*2 超出INT_MAX，导致size_t溢出为负数
         */
        if (needed <= INT_MAX)
        {
            newsize = INT_MAX; // 所需空间≤INT_MAX，直接用INT_MAX作为新长度
        }
        else
        {
            return NULL; // 所需空间超出INT_MAX，扩容失败
        }
    }
    else
    {
        /* 场景2：常规扩容 - 新长度=所需空间*2（2倍扩容，减少后续扩容次数） */
        newsize = needed * 2;
    }

    /* 第七步：执行缓冲区扩容（兼容自定义内存钩子） */
    if (p->hooks.reallocate != NULL)
    {
        /* 7.1 优先使用自定义reallocate钩子 - 高效扩容（原地重分配，无需拷贝数据）
         * realloc优势：若原缓冲区后有连续内存，直接扩展，性能远高于allocate+memcpy
         */
        newbuffer = (unsigned char*)p->hooks.reallocate(p->buffer, newsize);
        if (newbuffer == NULL)
        {
            /* 扩容失败：释放原缓冲区，重置缓冲区状态，避免内存泄漏 */
            p->hooks.deallocate(p->buffer);
            p->length = 0;
            p->buffer = NULL;
            return NULL;
        }
    }
    else
    {
        /* 7.2 无reallocate钩子：手动扩容（allocate新缓冲区 + memcpy数据 + 释放原缓冲区）
         * 通用方案，兼容所有内存管理器，但需要拷贝数据，性能略低
         */
        newbuffer = (unsigned char*)p->hooks.allocate(newsize);
        if (!newbuffer)
        {
            /* 分配失败：释放原缓冲区，重置状态 */
            p->hooks.deallocate(p->buffer);
            p->length = 0;
            p->buffer = NULL;
            return NULL;
        }

        /* 拷贝原缓冲区数据到新缓冲区（仅拷贝已写入的offset+1字节，包含\0） */
        memcpy(newbuffer, p->buffer, p->offset + 1);
        /* 释放原缓冲区，避免内存泄漏 */
        p->hooks.deallocate(p->buffer);
    }

    /* 第八步：更新缓冲区状态 - 指向新缓冲区，更新总长度 */
    p->length = newsize;
    p->buffer = newbuffer;

    /* 返回新缓冲区的当前偏移位置（可直接写入数据） */
    return newbuffer + p->offset;
}

/* calculate the new length of the string in a printbuffer and update the offset */
static void update_offset(printbuffer * const buffer)
{
    const unsigned char *buffer_pointer = NULL;
    if ((buffer == NULL) || (buffer->buffer == NULL))
    {
        return;
    }
    buffer_pointer = buffer->buffer + buffer->offset;

    buffer->offset += strlen((const char*)buffer_pointer);
}

/* securely comparison of floating-point variables */
static cJSON_bool compare_double(double a, double b)
{
    double maxVal = fabs(a) > fabs(b) ? fabs(a) : fabs(b);
    return (fabs(a - b) <= maxVal * DBL_EPSILON);
}

/* Render the number nicely from the given item into a string. */
static cJSON_bool print_number(const cJSON * const item, printbuffer * const output_buffer)
{
    unsigned char *output_pointer = NULL;
    double d = item->valuedouble;
    int length = 0;
    size_t i = 0;
    unsigned char number_buffer[26] = {0}; /* temporary buffer to print the number into */
    unsigned char decimal_point = get_decimal_point();
    double test = 0.0;

    if (output_buffer == NULL)
    {
        return false;
    }

    /* This checks for NaN and Infinity */
    if (isnan(d) || isinf(d))
    {
        length = sprintf((char*)number_buffer, "null");
    }
    else if(d == (double)item->valueint)
    {
        length = sprintf((char*)number_buffer, "%d", item->valueint);
    }
    else
    {
        /* Try 15 decimal places of precision to avoid nonsignificant nonzero digits */
        length = sprintf((char*)number_buffer, "%1.15g", d);

        /* Check whether the original double can be recovered */
        if ((sscanf((char*)number_buffer, "%lg", &test) != 1) || !compare_double((double)test, d))
        {
            /* If not, print with 17 decimal places of precision */
            length = sprintf((char*)number_buffer, "%1.17g", d);
        }
    }

    /* sprintf failed or buffer overrun occurred */
    if ((length < 0) || (length > (int)(sizeof(number_buffer) - 1)))
    {
        return false;
    }

    /* reserve appropriate space in the output */
    output_pointer = ensure(output_buffer, (size_t)length + sizeof(""));
    if (output_pointer == NULL)
    {
        return false;
    }

    /* copy the printed number to the output and replace locale
     * dependent decimal point with '.' */
    for (i = 0; i < ((size_t)length); i++)
    {
        if (number_buffer[i] == decimal_point)
        {
            output_pointer[i] = '.';
            continue;
        }

        output_pointer[i] = number_buffer[i];
    }
    output_pointer[i] = '\0';

    output_buffer->offset += (size_t)length;

    return true;
}

/* parse 4 digit hexadecimal number */
static unsigned parse_hex4(const unsigned char * const input)
{
    unsigned int h = 0;
    size_t i = 0;

    for (i = 0; i < 4; i++)
    {
        /* parse digit */
        if ((input[i] >= '0') && (input[i] <= '9'))
        {
            h += (unsigned int) input[i] - '0';
        }
        else if ((input[i] >= 'A') && (input[i] <= 'F'))
        {
            h += (unsigned int) 10 + input[i] - 'A';
        }
        else if ((input[i] >= 'a') && (input[i] <= 'f'))
        {
            h += (unsigned int) 10 + input[i] - 'a';
        }
        else /* invalid */
        {
            return 0;
        }

        if (i < 3)
        {
            /* shift left to make place for the next nibble */
            h = h << 4;
        }
    }

    return h;
}

/* converts a UTF-16 literal to UTF-8
 * A literal can be one or two sequences of the form \uXXXX */
static unsigned char utf16_literal_to_utf8(const unsigned char * const input_pointer, const unsigned char * const input_end, unsigned char **output_pointer)
{
    long unsigned int codepoint = 0;
    unsigned int first_code = 0;
    const unsigned char *first_sequence = input_pointer;
    unsigned char utf8_length = 0;
    unsigned char utf8_position = 0;
    unsigned char sequence_length = 0;
    unsigned char first_byte_mark = 0;

    if ((input_end - first_sequence) < 6)
    {
        /* input ends unexpectedly */
        goto fail;
    }

    /* get the first utf16 sequence */
    first_code = parse_hex4(first_sequence + 2);

    /* check that the code is valid */
    if (((first_code >= 0xDC00) && (first_code <= 0xDFFF)))
    {
        goto fail;
    }

    /* UTF16 surrogate pair */
    if ((first_code >= 0xD800) && (first_code <= 0xDBFF))
    {
        const unsigned char *second_sequence = first_sequence + 6;
        unsigned int second_code = 0;
        sequence_length = 12; /* \uXXXX\uXXXX */

        if ((input_end - second_sequence) < 6)
        {
            /* input ends unexpectedly */
            goto fail;
        }

        if ((second_sequence[0] != '\\') || (second_sequence[1] != 'u'))
        {
            /* missing second half of the surrogate pair */
            goto fail;
        }

        /* get the second utf16 sequence */
        second_code = parse_hex4(second_sequence + 2);
        /* check that the code is valid */
        if ((second_code < 0xDC00) || (second_code > 0xDFFF))
        {
            /* invalid second half of the surrogate pair */
            goto fail;
        }


        /* calculate the unicode codepoint from the surrogate pair */
        codepoint = 0x10000 + (((first_code & 0x3FF) << 10) | (second_code & 0x3FF));
    }
    else
    {
        sequence_length = 6; /* \uXXXX */
        codepoint = first_code;
    }

    /* encode as UTF-8
     * takes at maximum 4 bytes to encode:
     * 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx */
    if (codepoint < 0x80)
    {
        /* normal ascii, encoding 0xxxxxxx */
        utf8_length = 1;
    }
    else if (codepoint < 0x800)
    {
        /* two bytes, encoding 110xxxxx 10xxxxxx */
        utf8_length = 2;
        first_byte_mark = 0xC0; /* 11000000 */
    }
    else if (codepoint < 0x10000)
    {
        /* three bytes, encoding 1110xxxx 10xxxxxx 10xxxxxx */
        utf8_length = 3;
        first_byte_mark = 0xE0; /* 11100000 */
    }
    else if (codepoint <= 0x10FFFF)
    {
        /* four bytes, encoding 1110xxxx 10xxxxxx 10xxxxxx 10xxxxxx */
        utf8_length = 4;
        first_byte_mark = 0xF0; /* 11110000 */
    }
    else
    {
        /* invalid unicode codepoint */
        goto fail;
    }

    /* encode as utf8 */
    for (utf8_position = (unsigned char)(utf8_length - 1); utf8_position > 0; utf8_position--)
    {
        /* 10xxxxxx */
        (*output_pointer)[utf8_position] = (unsigned char)((codepoint | 0x80) & 0xBF);
        codepoint >>= 6;
    }
    /* encode first byte */
    if (utf8_length > 1)
    {
        (*output_pointer)[0] = (unsigned char)((codepoint | first_byte_mark) & 0xFF);
    }
    else
    {
        (*output_pointer)[0] = (unsigned char)(codepoint & 0x7F);
    }

    *output_pointer += utf8_length;

    return sequence_length;

fail:
    return 0;
}
/*
 * 函数名：parse_string
 * 功能：JSON字符串解析核心函数（私有静态）- 处理带转义的JSON字符串解析
 *       解析双引号包裹的JSON字符串，处理转义字符（\b/\f/\n/\r/\t/\"/\\/\//uXXXX），将UTF16转UTF8，最终填充到cJSON节点
 * 核心设计思路：
 *       1. 两步解析法：先遍历计算字符串长度（预分配内存）→ 再遍历处理转义字符（填充内容），避免多次内存重分配；
 *       2. 内存安全：预分配时“高估”内存大小（扣除转义字符占用），失败时自动回滚释放内存；
 *       3. 转义处理：覆盖JSON标准所有转义序列，UTF16转UTF8保证多语言兼容；
 *       4. 边界防护：全程校验缓冲区长度，避免内存越界，异常时通过goto统一处理失败逻辑。
 *
 * 参数：
 *   item          - cJSON* const，待填充的cJSON节点（解析结果写入type/valuestring字段）
 *   input_buffer  - parse_buffer* const，JSON解析缓冲区（包含待解析内容、偏移、内存钩子等）
 *
 * 返回值：
 *   cJSON_True(1) - 解析成功（字符串合法，节点填充完成）；
 *   cJSON_False(0) - 解析失败（格式错误/内存不足/转义无效/缓冲区越界）。
 *
 * 关键术语：
 *   - 转义序列：JSON中以\开头的字符（如\"表示双引号，\uXXXX表示UTF16字符）；
 *   - UTF16-literal：\u后接4位十六进制数（如\u4E2D），需转为UTF8编码存储；
 *   - allocation_length：输出字符串的预分配长度（原字符串长度 - 转义字符占用的额外字节）。
 */
/* Parse the input text into an unescaped cinput, and populate item. */
static cJSON_bool parse_string(cJSON * const item, parse_buffer * const input_buffer)
{
    //输入指针：指向当前解析位置（初始为第一个字符后，即跳过开头的双引号）
    const unsigned char *input_pointer = buffer_at_offset(input_buffer) + 1;
    //输入结束指针：指向当前解析位置的下一个字符（初始为第一个字符后，即跳过开头的双引号）
    const unsigned char *input_end = buffer_at_offset(input_buffer) + 1;
    //输出指针：指向当前输出位置（初始为NULL，后续指向预分配的输出缓冲区）
    unsigned char *output_pointer = NULL;
    //输出缓冲区：预分配的内存，用于存储解析后的字符串（不含转义字符），最终赋值给item->valuestring
    unsigned char *output = NULL;
    //第一步：快速校验 - 当前字符不是双引号，直接判定不是JSON字符串，跳转到失败逻辑
    /* not a string */
    if (buffer_at_offset(input_buffer)[0] != '\"')
    {
        goto fail;
    }

    {
        //第二步：预遍历字符串，计算输出缓冲区所需长度（避免多次内存分配）
        /* calculate approximate size of the output (overestimate) */
        //预分配长度：最终输出字符串的最大长度（高估，确保足够）
        size_t allocation_length = 0;
        //跳过的字节数：转义字符占用的额外字节（如\uXXXX占6字节，转UTF8后占2-4字节，需扣除差值）
        size_t skipped_bytes = 0;
        //循环条件：当前输入位置未超出缓冲区长度且未遇到字符串结束的双引号
        while (((size_t)(input_end - input_buffer->content) < input_buffer->length) && (*input_end != '\"'))
        {
            /* is escape sequence */
            //如果当前字符是转义字符（\），则根据转义类型计算跳过的字节数，调整输入结束指针
            if (input_end[0] == '\\')
            {
                //根据转义类型计算跳过的字节数，\uXXXX占6字节，转UTF8后占2-4字节，需扣除差值
                if ((size_t)(input_end + 1 - input_buffer->content) >= input_buffer->length)
                {
                    /* prevent buffer overflow when last input character is a backslash */
                    goto fail;
                }
                skipped_bytes++;//转义字符占用的额外字节数增加1
                input_end++;//跳过转义字符\
            }
            input_end++;//继续向后移动输入结束指针，直到遇到字符串结束的双引号或超出缓冲区长度
        }
        //校验结束条件：如果输入结束指针超出缓冲区长度或未遇到字符串结束的双引号，说明字符串格式错误，跳转到失败逻辑
        if (((size_t)(input_end - input_buffer->content) >= input_buffer->length) || (*input_end != '\"'))
        {
            goto fail; /* string ended unexpectedly 字符串意外结束*/
        }

        /* This is at most how much we need for the output */
        //预分配长度：原字符串长度 - 转义字符占用的额外字节数（高估，确保足够）
        allocation_length = (size_t) (input_end - buffer_at_offset(input_buffer)) - skipped_bytes;
        //内存分配 - 根据预估长度通过内存钩子分配输出缓冲区，失败时跳转到失败逻辑
        output = (unsigned char*)input_buffer->hooks.allocate(allocation_length + sizeof(""));
        if (output == NULL)
        {
            goto fail; /* allocation failure 内存分配失败*/
        }
    }
    //处理转义字符，填充输出缓冲区
    output_pointer = output;
    /* loop through the string literal */
    //循环条件：当前输入位置未超出缓冲区长度且未遇到字符串结束的双引号
    while (input_pointer < input_end)
    {
        //1.普通字符 - 直接复制到输出缓冲区，输入指针向后移动
        if (*input_pointer != '\\')
        {
            *output_pointer++ = *input_pointer++;
        }
        /* escape sequence */
        //2.转义字符 - 根据转义类型处理，转换为对应的字符或UTF8编码，填充到输出缓冲区，输入指针根据转义类型移动
        else
        {
            //转义序列长度：根据转义类型计算跳过的字节数，\uXXXX占6字节，转UTF8后占2-4字节，需扣除差值
            unsigned char sequence_length = 2;
            //校验转义序列 - 确保输入缓冲区有足够的字节可供解析转义序列，避免越界访问，失败时跳转到失败逻辑
            if ((input_end - input_pointer) < 1)
            {
                goto fail;
            }
        //根据转义类型处理，转换为对应的字符或UTF8编码，填充到输出缓冲区
            switch (input_pointer[1])
            {
                //JSON标准转义字符处理
                case 'b'://\b - 退格符
                    *output_pointer++ = '\b';
                    break;
                case 'f'://\f - 换页符
                    *output_pointer++ = '\f';
                    break;
                case 'n'://\n - 换行符
                    *output_pointer++ = '\n';
                    break;
                case 'r'://\r - 回车符
                    *output_pointer++ = '\r';
                    break;
                case 't'://\t - 制表符
                    *output_pointer++ = '\t';
                    break;
                case '\"'://\" - 双引号
                case '\\'://\\ - 反斜杠
                case '/':/// - 正斜杠
                    *output_pointer++ = input_pointer[1];
                    break;
                //其他转义字符无效，跳转到失败逻辑
                /* UTF-16 literal */
                case 'u':
                //UTF16转UTF8处理 - 调用utf16_literal_to_utf8函数将UTF16-literal转换为UTF8编码，填充到输出缓冲区，输入指针根据转义类型移动
                //utf16_literal_to_utf8函数内部会校验UTF16-literal的格式和有效性，失败时返回0，跳转到失败逻辑
                    sequence_length = utf16_literal_to_utf8(input_pointer, input_end, &output_pointer);
                    if (sequence_length == 0)
                    {
                        /* failed to convert UTF16-literal to UTF-8 */
                        goto fail;//UTF16-literal转换失败，可能是格式错误或无效的Unicode码点，跳转到失败逻辑
                    }
                    break;
                //其他转义字符无效，跳转到失败逻辑
                default:
                    goto fail;
            }
            //根据转义类型移动输入指针，继续处理下一个字符
            input_pointer += sequence_length;
        }
    }

    /* zero terminate the output */
    //输出字符串末尾添加'\0'，确保字符串正确结束
    *output_pointer = '\0';
    //填充cJSON节点 - 设置节点类型为字符串，赋值解析后的字符串到valuestring字段
    item->type = cJSON_String;
    item->valuestring = (char*)output;
    //更新输入缓冲区偏移 - 将输入缓冲区的偏移更新到当前解析位置（跳过结尾的双引号），确保后续解析从正确位置开始
    input_buffer->offset = (size_t) (input_end - input_buffer->content);
    input_buffer->offset++;
    //解析成功，返回true
    return true;
//解析失败统一处理逻辑 - 释放已分配的输出缓冲区，重置输入缓冲区偏移，返回false
fail:
    //释放已分配的输出缓冲区
    if (output != NULL)
    {
        input_buffer->hooks.deallocate(output);
        output = NULL;
    }
    //重置输入缓冲区偏移 - 将输入缓冲区的偏移重置到解析前的位置，确保后续解析不受影响
    if (input_pointer != NULL)
    {
        input_buffer->offset = (size_t)(input_pointer - input_buffer->content);
    }
    //返回false，表示解析失败
    return false;
}

/* Render the cstring provided to an escaped version that can be printed. */
static cJSON_bool print_string_ptr(const unsigned char * const input, printbuffer * const output_buffer)
{
    const unsigned char *input_pointer = NULL;
    unsigned char *output = NULL;
    unsigned char *output_pointer = NULL;
    size_t output_length = 0;
    /* numbers of additional characters needed for escaping */
    size_t escape_characters = 0;

    if (output_buffer == NULL)
    {
        return false;
    }

    /* empty string */
    if (input == NULL)
    {
        output = ensure(output_buffer, sizeof("\"\""));
        if (output == NULL)
        {
            return false;
        }
        strcpy((char*)output, "\"\"");

        return true;
    }

    /* set "flag" to 1 if something needs to be escaped */
    for (input_pointer = input; *input_pointer; input_pointer++)
    {
        switch (*input_pointer)
        {
            case '\"':
            case '\\':
            case '\b':
            case '\f':
            case '\n':
            case '\r':
            case '\t':
                /* one character escape sequence */
                escape_characters++;
                break;
            default:
                if (*input_pointer < 32)
                {
                    /* UTF-16 escape sequence uXXXX */
                    escape_characters += 5;
                }
                break;
        }
    }
    output_length = (size_t)(input_pointer - input) + escape_characters;

    output = ensure(output_buffer, output_length + sizeof("\"\""));
    if (output == NULL)
    {
        return false;
    }

    /* no characters have to be escaped */
    if (escape_characters == 0)
    {
        output[0] = '\"';
        memcpy(output + 1, input, output_length);
        output[output_length + 1] = '\"';
        output[output_length + 2] = '\0';

        return true;
    }

    output[0] = '\"';
    output_pointer = output + 1;
    /* copy the string */
    for (input_pointer = input; *input_pointer != '\0'; (void)input_pointer++, output_pointer++)
    {
        if ((*input_pointer > 31) && (*input_pointer != '\"') && (*input_pointer != '\\'))
        {
            /* normal character, copy */
            *output_pointer = *input_pointer;
        }
        else
        {
            /* character needs to be escaped */
            *output_pointer++ = '\\';
            switch (*input_pointer)
            {
                case '\\':
                    *output_pointer = '\\';
                    break;
                case '\"':
                    *output_pointer = '\"';
                    break;
                case '\b':
                    *output_pointer = 'b';
                    break;
                case '\f':
                    *output_pointer = 'f';
                    break;
                case '\n':
                    *output_pointer = 'n';
                    break;
                case '\r':
                    *output_pointer = 'r';
                    break;
                case '\t':
                    *output_pointer = 't';
                    break;
                default:
                    /* escape and print as unicode codepoint */
                    sprintf((char*)output_pointer, "u%04x", *input_pointer);
                    output_pointer += 4;
                    break;
            }
        }
    }
    output[output_length + 1] = '\"';
    output[output_length + 2] = '\0';

    return true;
}

/* Invoke print_string_ptr (which is useful) on an item. */
static cJSON_bool print_string(const cJSON * const item, printbuffer * const p)
{
    return print_string_ptr((unsigned char*)item->valuestring, p);
}

/* Predeclare these prototypes. */
static cJSON_bool parse_value(cJSON * const item, parse_buffer * const input_buffer);
static cJSON_bool print_value(const cJSON * const item, printbuffer * const output_buffer);
static cJSON_bool parse_array(cJSON * const item, parse_buffer * const input_buffer);
static cJSON_bool print_array(const cJSON * const item, printbuffer * const output_buffer);
static cJSON_bool parse_object(cJSON * const item, parse_buffer * const input_buffer);
static cJSON_bool print_object(const cJSON * const item, printbuffer * const output_buffer);
/*
 * 函数名：buffer_skip_whitespace
 * 功能：JSON解析辅助函数（私有静态）- 跳过缓冲区中的空白字符
 *       空白字符定义为ASCII码≤32的字符（空格、制表符、换行、回车等），处理缓冲区边界防止越界
 * 核心步骤：
 *   1. 参数校验 → 2. 缓冲区可访问性校验 → 3. 循环跳过空白字符 → 4. 边界修正 → 5. 返回处理后的缓冲区
 *
 * 参数：
 *   buffer - parse_buffer* const，待处理的解析缓冲区结构体（包含内容、偏移、长度等信息）
 *
 * 返回值：
 *   parse_buffer* - 处理后的缓冲区指针（跳过空白字符后的偏移）；NULL表示参数非法
 *
 * 关键定义：
 *   - 空白字符：ASCII码≤32的字符（包含空格(32)、制表符(\t=9)、换行(\n=10)、回车(\r=13)、空字符(\0=0)等）；
 *   - parse_buffer结构体核心字段：
 *     content - 缓冲区内容指针；offset - 当前读取偏移；length - 缓冲区总长度。
 *
 * 注意事项：
 *   1. 边界安全：全程通过can_access_at_index校验缓冲区可访问性，避免内存越界；
 *   2. 偏移修正：若跳过空白后偏移等于缓冲区长度，回退1位（防止后续访问越界）；
 *   3. 无修改拷贝：仅修改缓冲区的offset字段，不修改content内容；
 *   4. 空值兼容：缓冲区内容为NULL/缓冲区不可访问时，返回NULL/原缓冲区，不崩溃。
 */
/* Utility to jump whitespace and cr/lf */
static parse_buffer *buffer_skip_whitespace(parse_buffer * const buffer)
{
    //step1:参数校验 - 检查输入缓冲区和内容合法性，空值直接返回NULL
    if ((buffer == NULL) || (buffer->content == NULL))
    {
        return NULL;
    }
    //step2:缓冲区可访问性校验 - 检查缓冲区起始位置是否可访问，若不可访问直接返回原缓冲区（不修改）
    if (cannot_access_at_index(buffer, 0))
    {
        return buffer;
    }
    //step3:循环跳过空白字符 - 通过can_access_at_index校验当前偏移可访问，并且当前字符ASCII码≤32时，持续增加偏移跳过空白
    while (can_access_at_index(buffer, 0) && (buffer_at_offset(buffer)[0] <= 32))
    {
       buffer->offset++;
    }
    //条件1：跳过空白后偏移等于缓冲区长度，说明已经到达末尾，回退1位（防止后续访问越界）
    if (buffer->offset == buffer->length)
    {
        buffer->offset--;
    }
    //条件2：跳过空白后偏移超过缓冲区长度，说明越界了，回退1位（防止后续访问越界）
    else if (buffer->offset > buffer->length)
    {
        buffer->offset = buffer->length - 1;
    }
    //step4:返回处理后的缓冲区 - 返回跳过空白字符后的缓冲区指针，供后续解析使用
    return buffer;
}

/* skip the UTF-8 BOM (byte order mark) if it is at the beginning of a buffer */
static parse_buffer *skip_utf8_bom(parse_buffer * const buffer)
{
    if ((buffer == NULL) || (buffer->content == NULL) || (buffer->offset != 0))
    {
        return NULL;
    }

    if (can_access_at_index(buffer, 4) && (strncmp((const char*)buffer_at_offset(buffer), "\xEF\xBB\xBF", 3) == 0))
    {
        buffer->offset += 3;
    }

    return buffer;
}
/*
 * 函数名：cJSON_ParseWithLengthOpts
 * 功能：cJSON库底层核心解析函数，支持指定字符串长度和解析选项的JSON解析
 *       所有上层解析接口（cJSON_Parse/cJSON_ParseWithOpts）最终都调用此函数
 * 核心步骤：
 *   1. 初始化：重置全局错误信息，初始化解析缓冲区结构体
 *   2. 参数校验：检查输入字符串和长度合法性，空值直接失败
 *   3. 缓冲区配置：填充解析缓冲区的内容、长度、内存钩子等核心参数
 *   4. 根节点创建：为解析结果分配根节点内存，分配失败则跳转失败处理
 *   5. 核心解析：跳过BOM头和空白字符后，调用parse_value解析JSON值
 *   6. 合法性校验：若要求字符串以'\0'结尾，校验末尾是否符合要求
 *   7. 结果处理：返回解析后的根节点，或跳转失败流程释放资源并返回NULL
 * 参数：
 *   value                - const char*，待解析的JSON字符串，不可为NULL（空值直接失败）
 *   buffer_length        - size_t，JSON字符串的总长度（包含可能的'\0'）
 *   return_parse_end     - const char**，可选输出参数，用于返回解析结束的位置，可为NULL
 *   require_null_terminated - cJSON_bool，是否要求字符串必须以'\0'结尾（1=要求，0=不要求）
 * 返回值：
 *   成功 - cJSON*，指向解析后cJSON树形结构的根节点指针
 *   失败 - NULL，全局变量global_error会记录错误位置和相关信息
 * 注意事项：
 *   1. 内存管理：解析成功后必须调用cJSON_Delete释放根节点，否则造成内存泄漏
 *   2. 错误处理：失败时会自动释放已分配的根节点，无需手动处理
 *   3. 编码支持：自动跳过UTF-8 BOM头（\xEF\xBB\xBF），兼容带BOM的JSON字符串
 */
CJSON_PUBLIC(cJSON *) cJSON_ParseWithOpts(const char *value, const char **return_parse_end, cJSON_bool require_null_terminated)
{
    size_t buffer_length;

    if (NULL == value)
    {
        return NULL;
    }

    /* Adding null character size due to require_null_terminated. */
    buffer_length = strlen(value) + sizeof("");

    return cJSON_ParseWithLengthOpts(value, buffer_length, return_parse_end, require_null_terminated);
}

/* Parse an object - create a new root, and populate. */
CJSON_PUBLIC(cJSON *) cJSON_ParseWithLengthOpts(const char *value, size_t buffer_length, const char **return_parse_end, cJSON_bool require_null_terminated)
{
    parse_buffer buffer = { 0, 0, 0, 0, { 0, 0, 0 } };//初始化解析缓冲区：存储解析过程中的位置、长度、内存钩子等核心状态
    cJSON *item = NULL;//定义根结点指针并且初始化为NULL，解析成功后会指向解析结果的根节点

    /* reset error position */
    //重置全局错误信息：清空错误对应的JSON字符串指针和位置索引，为后续解析错误记录做好准备
    global_error.json = NULL;
    global_error.position = 0;

    //第一步：参数校验 - 检查输入字符串和长度合法性，空值直接失败
    //如果输入字符串为NULL或者长度为0，直接跳转到失败处理流程，避免后续解析过程中的非法访问和错误
    if (value == NULL || 0 == buffer_length)
    {
        goto fail;
    }

    //第二步：配置解析缓冲区 - 填充解析缓冲区的内容、长度、内存钩子等核心参数
    buffer.content = (const unsigned char*)value;//绑定带解析的字符串内容
    buffer.length = buffer_length;//设置字符串长度，解析过程中会根据这个长度进行边界检查
    buffer.offset = 0;//初始化解析位置索引为0，表示从字符串开头开始解析
    buffer.hooks = global_hooks;//使用全局默认的内存分配和释放函数，解析过程中会通过这些钩子进行内存管理

    //第三步：创建根节点 - 为解析结果分配根节点内存，分配失败则跳转失败处理
    item = cJSON_New_Item(&global_hooks);
    //内存分配失败，跳转到失败处理流程，在失败处理流程中会释放已分配的根节点（如果有）并记录错误信息
    if (item == NULL) /* memory fail */
    {
        goto fail;
    }

    //第四步：核心解析 - 跳过BOM头和空白字符后，调用parse_value解析JSON值
    //1. skip_utf8_bom函数会检查字符串开头是否有UTF-8 BOM头（\xEF\xBB\xBF），如果有则跳过这3个字节，兼容带BOM的JSON字符串
    //2. buffer_skip_whitespace函数会跳过字符串开头的空白字符（如空格、制表符、换行等），确保解析从第一个非空白字符开始
    //3. parse_value函数是核心解析函数，会根据当前解析位置的字符类型（对象、数组、字符串、数字等）递归解析JSON结构，并将结果存储在item指向的cJSON结构中
    if (!parse_value(item, buffer_skip_whitespace(skip_utf8_bom(&buffer))))
    {
        /* parse failure. ep is set. */
        //解析失败，跳转到失败处理流程（此时global_error记录了错误位置）
        goto fail;
    }

    //第五步：合法性校验 - 若要求字符串以'\0'结尾，校验末尾是否符合要求
    /* if we require null-terminated JSON without appended garbage, skip and then check for a null terminator */
    if (require_null_terminated)
    {
        buffer_skip_whitespace(&buffer);//跳过解析后剩余的空白字符
        //检查：1.解析偏移量是否超出长度2.末尾字符不是'\0'
        if ((buffer.offset >= buffer.length) || buffer_at_offset(&buffer)[0] != '\0')
        {
            goto fail;
        }
    }
    //第六步：设置解析结束位置（return_parse_end不为空）
    if (return_parse_end)
    {
        //将解析结束的位置赋值给输出参数
        *return_parse_end = (const char*)buffer_at_offset(&buffer);
    }

    //解析成功，返回解析后的根节点指针
    return item;

    //失败处理流程：1.统一释放资源2.记录错误信息3.返回NULL
fail:
    //释放已经分配内存分配的根节点（如果分配成功）
    if (item != NULL)
    {
        cJSON_Delete(item);
    }

    //如果输入的字符串部位NULL，记录未知错误到全局错误变量
    if (value != NULL)
    {
        error local_error;
        //初始化本地错误信息：绑定错误对应的JSON字符串和位置索引
        local_error.json = (const unsigned char*)value;
        local_error.position = 0;

        //确定错误位置：如果解析偏移量在字符串长度范围内，使用当前偏移量；否则如果字符串非空，使用最后一个字符位置
        if (buffer.offset < buffer.length)
        {
            local_error.position = buffer.offset;
        }
        else if (buffer.length > 0)
        {
            local_error.position = buffer.length - 1;
        }

        //如果需要返回解析结束位置，设置为错误位置
        if (return_parse_end != NULL)
        {
            *return_parse_end = (const char*)local_error.json + local_error.position;
        }

        //将本地错误信息同步给全局错误变量，供上层调用者查看
        global_error = local_error;
    }

    //解析失败：返回NULL
    return NULL;
}
/* Default options for cJSON_Parse */
/*
 * 函数名：cJSON_Parse
 * 功能：cJSON对外暴露的简化解析接口，使用默认选项解析JSON字符串
 * 核心逻辑：直接调用cJSON_ParseWithOpts，传入默认参数（无错误位置返回、不要求'\0'结尾）
 * 参数：
 *   value - const char*，待解析的JSON字符串，可为NULL（NULL时返回NULL）
 * 返回值：
 *   成功 - cJSON*，解析后的根节点指针；失败 - NULL
 * 注意事项：解析成功后需调用cJSON_Delete释放根节点，避免内存泄漏
 */
/* Default options for cJSON_Parse */
CJSON_PUBLIC(cJSON *) cJSON_Parse(const char *value)
{
    return cJSON_ParseWithOpts(value, 0, 0);
}
/*
 * 函数名：cJSON_ParseWithLength
 * 功能：简化版带长度的解析接口，使用默认选项解析指定长度的JSON字符串
 * 核心逻辑：调用cJSON_ParseWithLengthOpts，传入默认参数（无错误位置返回、不要求'\0'结尾）
 * 参数：
 *   value - const char*，待解析的JSON字符串；buffer_length - 字符串长度
 * 返回值：
 *   成功 - cJSON*，解析后的根节点指针；失败 - NULL
 * 适用场景：解析非'\0'结尾的字符串（如从缓冲区读取的固定长度数据）
 */
CJSON_PUBLIC(cJSON *) cJSON_ParseWithLength(const char *value, size_t buffer_length)
{
    return cJSON_ParseWithLengthOpts(value, buffer_length, 0, 0);
}

#define cjson_min(a, b) (((a) < (b)) ? (a) : (b))

static unsigned char *print(const cJSON * const item, cJSON_bool format, const internal_hooks * const hooks)
{
    static const size_t default_buffer_size = 256;
    printbuffer buffer[1];
    unsigned char *printed = NULL;

    memset(buffer, 0, sizeof(buffer));

    /* create buffer */
    buffer->buffer = (unsigned char*) hooks->allocate(default_buffer_size);
    buffer->length = default_buffer_size;
    buffer->format = format;
    buffer->hooks = *hooks;
    if (buffer->buffer == NULL)
    {
        goto fail;
    }

    /* print the value */
    if (!print_value(item, buffer))
    {
        goto fail;
    }
    update_offset(buffer);

    /* check if reallocate is available */
    if (hooks->reallocate != NULL)
    {
        printed = (unsigned char*) hooks->reallocate(buffer->buffer, buffer->offset + 1);
        if (printed == NULL) {
            goto fail;
        }
        buffer->buffer = NULL;
    }
    else /* otherwise copy the JSON over to a new buffer */
    {
        printed = (unsigned char*) hooks->allocate(buffer->offset + 1);
        if (printed == NULL)
        {
            goto fail;
        }
        memcpy(printed, buffer->buffer, cjson_min(buffer->length, buffer->offset + 1));
        printed[buffer->offset] = '\0'; /* just to be sure */

        /* free the buffer */
        hooks->deallocate(buffer->buffer);
        buffer->buffer = NULL;
    }

    return printed;

fail:
    if (buffer->buffer != NULL)
    {
        hooks->deallocate(buffer->buffer);
        buffer->buffer = NULL;
    }

    if (printed != NULL)
    {
        hooks->deallocate(printed);
        printed = NULL;
    }

    return NULL;
}
/*
 * 函数名：cJSON_Print
 * 功能：将cJSON节点转换为格式化的JSON字符串（核心序列化函数）
 *       输出的字符串带缩进、换行，可读性强，底层委托print函数实现
 * 核心步骤：
 *   1. 委托底层函数：调用print函数，传入待序列化的item、格式化标识（true）、全局内存钩子；
 *   2. 类型转换：将print返回的unsigned char*转换为char*，适配对外接口；
 *   3. 返回结果：成功返回JSON字符串指针，失败返回NULL。
 * 参数：
 *   item - const cJSON*，待序列化的cJSON节点（可为NULL，NULL时返回空字符串或NULL，取决于底层实现）
 * 返回值：
 *   成功 - char*，指向格式化JSON字符串的指针（内存由global_hooks分配，需调用cJSON_free释放）；
 *   失败 - NULL，原因：节点非法/内存分配失败/序列化逻辑出错。
 * 注意事项：
 *   1. 内存管理：返回的字符串由cJSON_malloc（global_hooks）分配，必须调用cJSON_free释放，否则内存泄漏；
 *   2. 格式化输出：true参数表示输出带缩进、换行的“漂亮格式”，区别于无格式的紧凑输出；
 *   3. 类型兼容：支持所有cJSON节点类型（Object/Array/String/Number等）的序列化；
 *   4. NULL处理：传入NULL时，底层会安全处理，不会崩溃（通常返回空字符串或NULL）；
 *   5. 编码兼容：序列化后的字符串为UTF-8编码，兼容JSON标准。
 */
/* Render a cJSON item/entity/structure to text. */
CJSON_PUBLIC(char *) cJSON_Print(const cJSON *item)
{
    //调用底层print函数：true表示格式化输出，使用全局钩子分配内存，返回值转换为char*
    return (char*)print(item, true, &global_hooks);
}
/*
 * 函数名：cJSON_PrintUnformatted
 * 功能：将cJSON节点转换为无格式的紧凑JSON字符串（核心序列化函数）
 *       输出的字符串无缩进、无换行，体积更小，底层委托print函数实现
 * 核心步骤：
 *   1. 委托底层函数：调用print函数，传入待序列化的item、无格式标识（false）、全局内存钩子；
 *   2. 类型转换：将print返回的unsigned char*转换为char*，适配对外接口；
 *   3. 返回结果：成功返回紧凑JSON字符串指针，失败返回NULL。
 * 参数：
 *   item - const cJSON*，待序列化的cJSON节点（可为NULL，NULL时返回空字符串或NULL，取决于底层实现）
 * 返回值：
 *   成功 - char*，指向紧凑JSON字符串的指针（内存由global_hooks分配，需调用cJSON_free释放）；
 *   失败 - NULL，原因：节点非法/内存分配失败/序列化逻辑出错。
 * 注意事项：
 *   1. 内存管理：返回的字符串由cJSON_malloc分配，必须调用cJSON_free释放，否则内存泄漏（核心易错点）；
 *   2. 紧凑输出：false参数表示输出无缩进、无换行的“紧凑格式”，体积更小，适合网络传输/存储；
 *   3. 与cJSON_Print的区别：前者格式化（易读），后者紧凑（省空间），底层共用print函数仅格式参数不同；
 *   4. 类型兼容：支持所有cJSON节点类型（Object/Array/String/Number等）的序列化；
 *   5. NULL处理：传入NULL时底层安全处理，不会崩溃（通常返回空字符串或NULL）；
 *   6. 编码兼容：序列化后的字符串为UTF-8编码，符合JSON标准。
 */
CJSON_PUBLIC(char *) cJSON_PrintUnformatted(const cJSON *item)
{
    //调用底层print函数：false表示无格式输出，使用全局钩子分配内存，返回值转换为char*
    return (char*)print(item, false, &global_hooks);
}
/*
 * 函数名：cJSON_PrintBuffered
 * 功能：带预分配缓冲区的JSON序列化（高性能、可控内存版）
 *       先按指定大小预开缓冲区，再把JSON结构打印到缓冲区，支持格式化/紧凑两种模式
 * 核心步骤：
 *   1. 初始化打印缓冲区结构体 printbuffer，全部清零；
 *   2. 检查预分配大小 prebuffer，若为负直接返回 NULL；
 *   3. 使用全局内存钩子分配指定大小的缓冲区；
 *   4. 初始化缓冲区参数：长度、偏移、是否允许动态扩容、格式化标志、内存钩子；
 *   5. 调用 print_value 递归将整个JSON结构写入缓冲区；
 *   6. 打印失败则释放已分配的内存，返回 NULL；
 *   7. 打印成功则返回构造好的JSON字符串。
 *
 * 参数：
 *   item      - 要序列化的JSON根节点
 *   prebuffer - 预先分配的缓冲区大小（字节）
 *   fmt       - 是否格式化输出（true=带缩进换行，false=紧凑）
 *
 * 返回值：
 *   成功 - 返回JSON字符串（必须用 cJSON_free / global_hooks.deallocate 释放）
 *   失败 - NULL
 *
 * 注意事项：
 *   1. 这是 cJSON 序列化的**底层高性能接口**，cJSON_Print 和 cJSON_PrintUnformatted 都是基于它封装的；
 *   2. 预分配太小会自动扩容，预分配合适能显著提升性能；
 *   3. 返回的内存由 global_hooks 分配，必须配套释放，不能用普通 free；
 *   4. 失败时会自动回滚，释放已分配内存，不会内存泄漏。
 */
CJSON_PUBLIC(char *) cJSON_PrintBuffered(const cJSON *item, int prebuffer, cJSON_bool fmt)
{
    //初始化打印缓冲区结构体，全部成员清零
    printbuffer p = { 0, 0, 0, 0, 0, 0, { 0, 0, 0 } };

    //预分配的长度不能为负数
    if (prebuffer < 0)
    {
        return NULL;
    }

    //分配预缓冲内存
    p.buffer = (unsigned char*)global_hooks.allocate((size_t)prebuffer);
    if (!p.buffer)
    {
        return NULL;
    }

    //设置缓冲区的基本信息
    p.length = (size_t)prebuffer;//缓冲区总长度
    p.offset = 0;//当前写入偏移
    p.noalloc = false;//允许动态扩容
    p.format = fmt;//是否格式化输出
    p.hooks = global_hooks;//绑定全局内存钩子

    //递归打印JSON结构到缓冲区
    if (!print_value(item, &p))
    {
        //打印失败，释放已分配的内存，返回 NULL
        global_hooks.deallocate(p.buffer);
        p.buffer = NULL;
        return NULL;
    }

    //打印成功，返回构造好的JSON字符串
    return (char*)p.buffer;
}

CJSON_PUBLIC(cJSON_bool) cJSON_PrintPreallocated(cJSON *item, char *buffer, const int length, const cJSON_bool format)
{
    printbuffer p = { 0, 0, 0, 0, 0, 0, { 0, 0, 0 } };

    if ((length < 0) || (buffer == NULL))
    {
        return false;
    }

    p.buffer = (unsigned char*)buffer;
    p.length = (size_t)length;
    p.offset = 0;
    p.noalloc = true;
    p.format = format;
    p.hooks = global_hooks;

    return print_value(item, &p);
}
/*
 * 函数名：parse_value
 * 功能：JSON解析核心分发函数（私有静态）- 识别并解析JSON值类型
 *       根据缓冲区当前字符，匹配JSON的基础值类型（null/false/true/字符串/数值/数组/对象），分发到对应解析函数
 * 核心步骤：
 *   1. 参数校验 → 2. 依次匹配null/false/true → 3. 匹配字符串/数值/数组/对象并分发解析 → 4. 匹配失败返回false
 *
 * 参数：
 *   item          - cJSON* const，待填充的cJSON节点（解析结果会写入此节点的type/value等字段）
 *   input_buffer  - parse_buffer* const，JSON解析缓冲区（包含待解析内容、当前偏移、长度等）
 *
 * 返回值：
 *   cJSON_True(1) - 解析成功（匹配到合法JSON值并填充item）；
 *   cJSON_False(0) - 解析失败（无匹配的JSON值/参数非法/缓冲区不足）。
 *
 * JSON值类型匹配规则（按优先级）：
 *   1. 关键字类：null（4字符）、false（5字符）、true（4字符）→ 精确字符串匹配；
 *   2. 结构类：字符串（"开头）、数值（-/0-9开头）、数组（[开头）、对象（{开头）→ 首字符匹配+分发解析。
 *
 * 注意事项：
 *   1. 缓冲区安全：所有字符串匹配前先通过can_read校验缓冲区长度，避免内存越界；
 *   2. 偏移推进：匹配到关键字后，主动推进缓冲区偏移（跳过已解析的字符）；
 *   3. 优先级顺序：关键字类（null/false/true）优先于结构类，避免首字符误匹配；
 *   4. 仅填充节点：解析结果写入传入的item节点，不分配新节点，内存由上层管理；
 *   5. 无回溯设计：匹配失败直接返回false，回溯逻辑由上层解析函数处理。
 */
/* Parser core - when encountering text, process appropriately. */
static cJSON_bool parse_value(cJSON * const item, parse_buffer * const input_buffer)
{
    //step1:参数校验 - 确保输入的cJSON节点和解析缓冲区都不为NULL，否则直接返回false
    if ((input_buffer == NULL) || (input_buffer->content == NULL))
    {
        return false; /* no input */
    }

    /* parse the different types of values */
    /* null */
    //1.匹配null：先通过can_read校验缓冲区是否至少有4个字符可读，再通过strncmp精确匹配"null"字符串，匹配成功则设置item类型为cJSON_NULL，并推进偏移4个字符，返回true
    if (can_read(input_buffer, 4) && (strncmp((const char*)buffer_at_offset(input_buffer), "null", 4) == 0))
    {
        //can_read函数会检查input_buffer当前偏移位置是否至少有4个字节可读，避免越界访问
        //strncmp函数会比较input_buffer当前偏移位置的字符串与"null"是否
        item->type = cJSON_NULL;
        input_buffer->offset += 4;
        return true;
    }
    /* false */
    if (can_read(input_buffer, 5) && (strncmp((const char*)buffer_at_offset(input_buffer), "false", 5) == 0))
    {
        item->type = cJSON_False;//设置item类型为cJSON_False
        input_buffer->offset += 5;//推进缓冲区偏移5个字符（跳过"false"）
        return true;//匹配成功，返回true
    }
    /* true */
    //2.匹配true：先通过can_read校验缓冲区是否至少有4个字符可读，再通过strncmp精确匹配"true"字符串，匹配成功则设置item类型为cJSON_True，valueint为1，并推进偏移4个字符，返回true
    if (can_read(input_buffer, 4) && (strncmp((const char*)buffer_at_offset(input_buffer), "true", 4) == 0))
    {
        item->type = cJSON_True;//设置item类型为cJSON_True
        item->valueint = 1;//设置valueint为1
        input_buffer->offset += 4;//推进缓冲区偏移4个字符（跳过"true"）
        return true;
    }
    /* string */
    //3.匹配字符串：先通过can_access_at_index校验当前偏移位置是否可访问，并且当前字符是否为双引号（"），如果是则调用parse_string函数解析字符串，解析成功返回true，失败返回false
    if (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == '\"'))
    {
        return parse_string(item, input_buffer);
    }
    /* number */
    //4.匹配数值：先通过can_access_at_index校验当前偏移位置是否可访问，并且当前字符是否为数字（0-9）或负号（-），如果是则调用parse_number函数解析数值，解析成功返回true，失败返回false
    if (can_access_at_index(input_buffer, 0) && ((buffer_at_offset(input_buffer)[0] == '-') || ((buffer_at_offset(input_buffer)[0] >= '0') && (buffer_at_offset(input_buffer)[0] <= '9'))))
    {
        return parse_number(item, input_buffer);
    }
    /* array */
    //5.匹配数组：先通过can_access_at_index校验当前偏移位置是否可访问，并且当前字符是否为左方括号（[），如果是则调用parse_array函数解析数组，解析成功返回true，失败返回false
    if (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == '['))
    {
        return parse_array(item, input_buffer);
    }
    /* object */
    //6.匹配对象：先通过can_access_at_index校验当前偏移位置是否可访问，并且当前字符是否为左大括号（{），如果是则调用parse_object函数解析对象，解析成功返回true，失败返回false
    if (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == '{'))
    {
        return parse_object(item, input_buffer);
    }
    //7.匹配失败：如果以上所有类型匹配都失败了，说明当前字符不符合任何合法的JSON值类型，直接返回false
    return false;
}
/*
 * 函数名：print_value
 * 功能：核心序列化分发函数（私有静态函数）
 *       根据cJSON节点的不同类型，分发到对应的打印函数，完成JSON值的序列化
 * 核心逻辑：
 *       1. 参数校验 → 2. 按节点类型分支 → 3. 分配缓冲区 → 4. 写入对应JSON值 → 5. 返回执行结果
 *
 * 参数：
 *   item          - const cJSON* const，待序列化的cJSON节点（不可修改）
 *   output_buffer - printbuffer* const，打印缓冲区结构体（管理序列化的内存缓冲区）
 *
 * 返回值：
 *   cJSON_True(1) - 序列化成功；cJSON_False(0) - 序列化失败（参数非法/缓冲区不足/类型不支持）
 *
 * 注意事项：
 *   1. 私有函数：仅内部调用，不对外暴露；
 *   2. 类型掩码：(item->type) & 0xFF 用于过滤高位标志，仅保留基础类型（如cJSON_Object/cJSON_Array等）；
 *   3. 缓冲区保障：通过ensure函数确保缓冲区有足够空间，避免内存越界；
 *   4. 失败快速返回：任何一步出错立即返回false，无内存泄漏风险；
 *   5. 分支覆盖：支持cJSON所有基础类型（NULL/布尔/数值/字符串/数组/对象等）。
 */
/* Render a value to text. */
static cJSON_bool print_value(const cJSON * const item, printbuffer * const output_buffer)
{
    unsigned char *output = NULL;
    //step1:参数校验 - 确保输入的cJSON节点和输出缓冲区都不为NULL，否则直接返回false
    if ((item == NULL) || (output_buffer == NULL))
    {
        return false;
    }
    //step2:类型分支 - 根据cJSON节点的类型（通过掩码过滤高位标志），分发到对应的序列化逻辑
    switch ((item->type) & 0xFF)
    {
        //1.NULL类型+序列化输出为"null"
        case cJSON_NULL:
            output = ensure(output_buffer, 5);//5字节：4个字符"null" + 1个结尾'\0'
            if (output == NULL)
            {
                return false;
            }
            strcpy((char*)output, "null");//将"null"字符串复制到输出缓冲区
            return true;
            //2.布尔类型+序列化输出为"true"/"false"
        case cJSON_False:
            output = ensure(output_buffer, 6);//6字节：5个字符"false" + 1个结尾'\0'
            if (output == NULL)
            {
                return false;
            }
            strcpy((char*)output, "false");
            return true;
            //3.布尔类型+序列化输出为"true"
        case cJSON_True:
            output = ensure(output_buffer, 5);//5字节：4个字符"true" + 1个结尾'\0'
            if (output == NULL)
            {
                return false;
            }
            strcpy((char*)output, "true");
            return true;
            //4.数值类型+序列化输出为对应的数字字符串（如"123.45"）
        case cJSON_Number:
            return print_number(item, output_buffer);
            //5.原始类型+直接输出valuestring内容（不加引号，不转义）
        case cJSON_Raw:
        {
            size_t raw_length = 0;
            if (item->valuestring == NULL)//原始字符串不能为空
            {
                return false;
            }
            //计算原始字符串长度+1（结尾'\0'），确保缓冲区足够大
            raw_length = strlen(item->valuestring) + sizeof("");
            output = ensure(output_buffer, raw_length);//分配缓冲区
            if (output == NULL)
            {
                return false;
            }
            memcpy(output, item->valuestring, raw_length);//直接拷贝原始字符
            return true;
        }
        //6.字符串类型+序列化输出为带引号、转义的字符串（如"\"Hello, World!\""})
        case cJSON_String:
            return print_string(item, output_buffer);
        //7.数组类型+递归调用print_array序列化数组元素
        case cJSON_Array:
            return print_array(item, output_buffer);
        //8.对象类型+递归调用print_object序列化键值对
        case cJSON_Object:
            return print_object(item, output_buffer);
        //9.其他类型（如cJSON_Invalid等）不支持序列化，直接返回false
        default:
            return false;
    }
}
/*
 * 函数名：parse_array
 * 功能：JSON数组解析核心函数（私有静态）- 解析JSON数组（[]包裹的逗号分隔值列表）
 *       支持空数组/嵌套数组，限制嵌套深度防止栈溢出，构建链表存储数组元素，兼容任意空白字符，最终填充到cJSON节点
 * 核心设计思路：
 *       1. 递归下降解析：调用parse_value解析数组元素（支持所有JSON值类型，包括嵌套数组/对象）；
 *       2. 嵌套防护：通过depth限制数组嵌套深度（CJSON_NESTING_LIMIT），避免栈溢出/内存耗尽；
 *       3. 链表构建：用单向链表存储数组元素（head/current_item管理链表头/尾），符合cJSON的核心数据结构设计；
 *       4. 空白兼容：解析前后自动跳过空白字符（空格/换行/制表符），符合JSON语法规范；
 *       5. 失败回滚：解析失败时自动释放已分配的数组元素节点，无内存泄漏。
 *
 * 参数：
 *   item          - cJSON* const，待填充的cJSON节点（解析结果写入type/child字段，child指向数组元素链表头）
 *   input_buffer  - parse_buffer* const，JSON解析缓冲区（包含待解析内容、偏移、深度、内存钩子等）
 *
 * 返回值：
 *   cJSON_True(1) - 解析成功（数组合法，节点填充完成）；
 *   cJSON_False(0) - 解析失败（嵌套过深/格式错误/内存不足/解析元素失败）。
 *
 * 关键术语：
 *   - 递归下降解析：解析数组时调用parse_value，parse_value又可能调用parse_array，实现嵌套数组解析；
 *   - CJSON_NESTING_LIMIT：cJSON定义的最大嵌套深度（默认1000），防止恶意JSON（如多层嵌套数组）导致栈溢出；
 *   - 双向链表：数组元素通过prev/next串联，head指向链表头，current_item指向链表尾。
 */
/* Build an array from input text. */
static cJSON_bool parse_array(cJSON * const item, parse_buffer * const input_buffer)
{
    cJSON *head = NULL; /* head of the linked list *//* 数组元素链表的头节点（管理整个数组的元素） */
    cJSON *current_item = NULL;/* 数组元素链表的当前尾节点（用于添加新元素） */
    /* 第一步：嵌套深度校验 - 防止恶意嵌套数组导致栈溢出/内存耗尽 */
    if (input_buffer->depth >= CJSON_NESTING_LIMIT)
    {
        return false; /* to deeply nested - 嵌套深度超出限制 */
    }
    input_buffer->depth++; // 进入数组解析，深度+1

    /* 第二步：校验数组开头 - 当前字符不是[，判定不是JSON数组 */
    if (buffer_at_offset(input_buffer)[0] != '[')
    {
        goto fail; /* not an array - 非数组格式 */
    }
    input_buffer->offset++; // 跳过[，指向数组内部
    buffer_skip_whitespace(input_buffer); // 跳过[后的空白字符（如空格/换行）
    /* 第三步：处理空数组 - 跳过空白后直接遇到]，判定为空数组 */
    if (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == ']'))
    {
        goto success; // 跳转到成功逻辑（无需解析元素）
    }

    /* 第四步：边界校验 - 跳过空白后缓冲区已到末尾，解析失败 */
    if (cannot_access_at_index(input_buffer, 0))
    {
        input_buffer->offset--; // 回退偏移，便于上层定位错误
        goto fail;
    }

    /* 第五步：偏移回退 - 为do-while循环做准备（循环内会先offset++） */
    input_buffer->offset--;

    /* 第六步：循环解析数组元素（逗号分隔）
     * do-while逻辑：先执行一次解析（处理第一个元素），再判断是否有逗号（处理后续元素）
     * 核心：每次循环解析一个元素，构建链表，直到遇到非逗号字符
     */
    do
    {
        /* 6.1 分配新节点 - 存储当前数组元素 */
        cJSON *new_item = cJSON_New_Item(&(input_buffer->hooks));
        if (new_item == NULL)
        {
            goto fail; /* allocation failure - 内存分配失败 */
        }

        /* 6.2 构建双向链表 - 将新节点添加到链表末尾 */
        if (head == NULL)
        {
            /* 链表为空：初始化头节点和当前尾节点 */
            current_item = head = new_item;
        }
        else
        {
            /* 链表非空：将新节点挂到尾节点后，更新尾节点 */
            current_item->next = new_item;
            new_item->prev = current_item; // 双向链表，维护prev指针
            current_item = new_item;
        }

        /* 6.3 解析当前元素的值（递归下降核心） */
        input_buffer->offset++; // 推进偏移，指向元素起始位置
        buffer_skip_whitespace(input_buffer); // 跳过元素前的空白字符
        // 调用parse_value解析元素（支持字符串/数值/数组/对象等所有类型，实现嵌套解析）
        if (!parse_value(current_item, input_buffer))
        {
            goto fail; /* failed to parse value - 元素解析失败 */
        }
        buffer_skip_whitespace(input_buffer); // 跳过元素后的空白字符
    }
    // 循环条件：当前字符是逗号（表示还有下一个元素），且缓冲区可访问
    while (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == ','));

    /* 第七步：校验数组结尾 - 循环结束后必须遇到]，否则格式错误 */
    if (cannot_access_at_index(input_buffer, 0) || buffer_at_offset(input_buffer)[0] != ']')
    {
        goto fail; /* expected end of array - 缺少数组结束符] */
    }

/* 成功逻辑：填充数组节点，恢复深度，更新偏移 */
success:
    input_buffer->depth--; // 退出数组解析，深度-1

    /* 链表闭环处理：数组的最后一个元素的next指向NULL，头节点的prev指向尾节点（cJSON特有设计） */
    if (head != NULL) {
        head->prev = current_item;
    }

    /* 填充cJSON数组节点 */
    item->type = cJSON_Array; // 标记节点类型为数组
    item->child = head;       // 数组节点的child指向元素链表头

    input_buffer->offset++; // 跳过]，指向数组后的字符

    return true; // 解析成功

/* 失败逻辑：释放已分配的数组元素链表，返回失败 */
fail:
    /* 释放链表所有节点（cJSON_Delete会递归释放整个链表），避免内存泄漏 */
    if (head != NULL)
    {
        cJSON_Delete(head);
    }

    return false; // 解析失败
}

/* Render an array to text */
static cJSON_bool print_array(const cJSON * const item, printbuffer * const output_buffer)
{
    unsigned char *output_pointer = NULL;
    size_t length = 0;
    cJSON *current_element = item->child;

    if (output_buffer == NULL)
    {
        return false;
    }

    /* Compose the output array. */
    /* opening square bracket */
    output_pointer = ensure(output_buffer, 1);
    if (output_pointer == NULL)
    {
        return false;
    }

    *output_pointer = '[';
    output_buffer->offset++;
    output_buffer->depth++;

    while (current_element != NULL)
    {
        if (!print_value(current_element, output_buffer))
        {
            return false;
        }
        update_offset(output_buffer);
        if (current_element->next)
        {
            length = (size_t) (output_buffer->format ? 2 : 1);
            output_pointer = ensure(output_buffer, length + 1);
            if (output_pointer == NULL)
            {
                return false;
            }
            *output_pointer++ = ',';
            if(output_buffer->format)
            {
                *output_pointer++ = ' ';
            }
            *output_pointer = '\0';
            output_buffer->offset += length;
        }
        current_element = current_element->next;
    }

    output_pointer = ensure(output_buffer, 2);
    if (output_pointer == NULL)
    {
        return false;
    }
    *output_pointer++ = ']';
    *output_pointer = '\0';
    output_buffer->depth--;

    return true;
}

/* Build an object from the text. */
/*
 * 函数名：parse_object
 * 功能：JSON对象解析核心函数（私有静态）- 解析JSON对象（{}包裹的逗号分隔键值对列表）
 *       支持空对象/嵌套对象，限制嵌套深度防止栈溢出，构建双向链表存储键值对，严格遵循“字符串键:值”语法，兼容任意空白字符
 * 核心设计思路：
 *       1. 递归下降解析：先解析字符串键（parse_string），再解析值（parse_value），支持嵌套对象/数组；
 *       2. 嵌套防护：通过depth限制对象嵌套深度（CJSON_NESTING_LIMIT），避免恶意JSON导致栈溢出/内存耗尽；
 *       3. 链表构建：用双向链表存储键值对（head/current_item管理链表头/尾），与数组解析逻辑统一；
 *       4. 键值映射：将解析的字符串键从valuestring迁移到string字段（cJSON对象节点的特有设计）；
 *       5. 失败回滚：解析失败时自动释放已分配的键值对节点，无内存泄漏，保证内存安全。
 *
 * 参数：
 *   item          - cJSON* const，待填充的cJSON节点（解析结果写入type/child字段，child指向键值对链表头）
 *   input_buffer  - parse_buffer* const，JSON解析缓冲区（包含待解析内容、偏移、深度、内存钩子等）
 *
 * 返回值：
 *   cJSON_True(1) - 解析成功（对象合法，节点填充完成）；
 *   cJSON_False(0) - 解析失败（嵌套过深/格式错误/内存不足/键/值解析失败）。
 *
 * 关键术语：
 *   - JSON对象语法：{ "key1": value1, "key2": value2 }，键必须是字符串，值为任意JSON类型；
 *   - 双向链表：键值对节点通过prev/next串联，head指向第一个键值对，current_item指向最后一个；
 *   - CJSON_NESTING_LIMIT：最大嵌套深度（默认1000），防止多层嵌套对象攻击。
 */
static cJSON_bool parse_object(cJSON * const item, parse_buffer * const input_buffer)
{
    cJSON *head = NULL;        /* 对象键值对链表的头节点（管理所有键值对） */
    cJSON *current_item = NULL;/* 对象键值对链表的当前尾节点（用于添加新键值对） */

    /* 第一步：嵌套深度校验 - 防止恶意嵌套对象导致栈溢出/内存耗尽 */
    if (input_buffer->depth >= CJSON_NESTING_LIMIT)
    {
        return false; /* to deeply nested - 嵌套深度超出限制 */
    }
    input_buffer->depth++; // 进入对象解析，嵌套深度+1

    /* 第二步：校验对象开头 - 当前字符不是{，判定不是JSON对象 */
    if (cannot_access_at_index(input_buffer, 0) || (buffer_at_offset(input_buffer)[0] != '{'))
    {
        goto fail; /* not an object - 非对象格式 */
    }

    input_buffer->offset++; // 跳过{，指向对象内部
    buffer_skip_whitespace(input_buffer); // 跳过{后的空白字符（空格/换行/制表符等）

    /* 第三步：处理空对象 - 跳过空白后直接遇到}，判定为空对象 */
    if (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == '}'))
    {
        goto success; // 跳转到成功逻辑（无需解析键值对）
    }

    /* 第四步：边界校验 - 跳过空白后缓冲区已到末尾，解析失败 */
    if (cannot_access_at_index(input_buffer, 0))
    {
        input_buffer->offset--; // 回退偏移，便于上层定位错误位置
        goto fail;
    }

    /* 第五步：偏移回退 - 为do-while循环做准备（循环内会先offset++） */
    input_buffer->offset--;

    /* 第六步：循环解析对象键值对（逗号分隔）
     * do-while逻辑：先执行一次解析（第一个键值对），再判断是否有逗号（处理后续键值对）
     * 核心：每次循环解析一个“字符串键:值”对，构建链表，直到遇到非逗号字符
     */
    do
    {
        /* 6.1 分配新节点 - 存储当前键值对（键存在string字段，值存在valuestring/valueint/child等字段） */
        cJSON *new_item = cJSON_New_Item(&(input_buffer->hooks));
        if (new_item == NULL)
        {
            goto fail; /* allocation failure - 内存分配失败 */
        }

        /* 6.2 构建双向链表 - 将新键值对节点添加到链表末尾 */
        if (head == NULL)
        {
            /* 链表为空：初始化头节点和当前尾节点 */
            current_item = head = new_item;
        }
        else
        {
            /* 链表非空：将新节点挂到尾节点后，更新尾节点 */
            current_item->next = new_item;
            new_item->prev = current_item; // 双向链表，维护prev指针
            current_item = new_item;
        }

        /* 6.3 边界校验 - 逗号后无内容，解析失败 */
        if (cannot_access_at_index(input_buffer, 1))
        {
            goto fail; /* nothing comes after the comma - 逗号后无有效键值对 */
        }

        /* 6.4 解析键名（必须是JSON字符串） */
        input_buffer->offset++; // 推进偏移，指向键名起始位置
        buffer_skip_whitespace(input_buffer); // 跳过键名前的空白字符
        // 调用parse_string解析键名（结果写入new_item->valuestring）
        if (!parse_string(current_item, input_buffer))
        {
            goto fail; /* failed to parse name - 键名解析失败（非合法字符串） */
        }
        buffer_skip_whitespace(input_buffer); // 跳过键名后的空白字符

        /* 6.5 键名字段迁移 - cJSON对象节点的特殊设计：
         * parse_string将键名写入valuestring，需迁移到string字段（string字段专门存储对象键名）
         * valuestring置空，为后续存储值预留空间
         */
        current_item->string = current_item->valuestring;
        current_item->valuestring = NULL;

        /* 6.6 校验键值分隔符 - 必须是冒号:，否则对象格式非法 */
        if (cannot_access_at_index(input_buffer, 0) || (buffer_at_offset(input_buffer)[0] != ':'))
        {
            goto fail; /* invalid object - 缺少键值分隔符: */
        }

        /* 6.7 解析键对应的值（递归下降核心） */
        input_buffer->offset++; // 跳过:，指向值起始位置
        buffer_skip_whitespace(input_buffer); // 跳过值前的空白字符
        // 调用parse_value解析值（支持字符串/数值/数组/对象等所有JSON类型，实现嵌套解析）
        if (!parse_value(current_item, input_buffer))
        {
            goto fail; /* failed to parse value - 值解析失败 */
        }
        buffer_skip_whitespace(input_buffer); // 跳过值后的空白字符
    }
    // 循环条件：当前字符是逗号（表示还有下一个键值对），且缓冲区可访问
    while (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == ','));

    /* 第七步：校验对象结尾 - 循环结束后必须遇到}，否则格式错误 */
    if (cannot_access_at_index(input_buffer, 0) || (buffer_at_offset(input_buffer)[0] != '}'))
    {
        goto fail; /* expected end of object - 缺少对象结束符} */
    }

/* 成功逻辑：填充对象节点，恢复嵌套深度，更新缓冲区偏移 */
success:
    input_buffer->depth--; // 退出对象解析，嵌套深度-1

    /* 链表闭环处理：头节点的prev指向尾节点（cJSON特有设计，便于反向遍历键值对） */
    if (head != NULL) {
        head->prev = current_item;
    }

    /* 填充cJSON对象节点 */
    item->type = cJSON_Object; // 标记节点类型为对象
    item->child = head;        // 对象节点的child指向键值对链表头

    input_buffer->offset++; // 跳过}，指向对象后的字符
    return true; // 解析成功

/* 失败逻辑：释放已分配的键值对链表，返回失败 */
fail:
    /* 释放链表所有节点（cJSON_Delete会递归释放整个链表），避免内存泄漏 */
    if (head != NULL)
    {
        cJSON_Delete(head);
    }

    return false; // 解析失败
}

/* Render an object to text. */
static cJSON_bool print_object(const cJSON * const item, printbuffer * const output_buffer)
{
    unsigned char *output_pointer = NULL;
    size_t length = 0;
    cJSON *current_item = item->child;

    if (output_buffer == NULL)
    {
        return false;
    }

    /* Compose the output: */
    length = (size_t) (output_buffer->format ? 2 : 1); /* fmt: {\n */
    output_pointer = ensure(output_buffer, length + 1);
    if (output_pointer == NULL)
    {
        return false;
    }

    *output_pointer++ = '{';
    output_buffer->depth++;
    if (output_buffer->format)
    {
        *output_pointer++ = '\n';
    }
    output_buffer->offset += length;

    while (current_item)
    {
        if (output_buffer->format)
        {
            size_t i;
            output_pointer = ensure(output_buffer, output_buffer->depth);
            if (output_pointer == NULL)
            {
                return false;
            }
            for (i = 0; i < output_buffer->depth; i++)
            {
                *output_pointer++ = '\t';
            }
            output_buffer->offset += output_buffer->depth;
        }

        /* print key */
        if (!print_string_ptr((unsigned char*)current_item->string, output_buffer))
        {
            return false;
        }
        update_offset(output_buffer);

        length = (size_t) (output_buffer->format ? 2 : 1);
        output_pointer = ensure(output_buffer, length);
        if (output_pointer == NULL)
        {
            return false;
        }
        *output_pointer++ = ':';
        if (output_buffer->format)
        {
            *output_pointer++ = '\t';
        }
        output_buffer->offset += length;

        /* print value */
        if (!print_value(current_item, output_buffer))
        {
            return false;
        }
        update_offset(output_buffer);

        /* print comma if not last */
        length = ((size_t)(output_buffer->format ? 1 : 0) + (size_t)(current_item->next ? 1 : 0));
        output_pointer = ensure(output_buffer, length + 1);
        if (output_pointer == NULL)
        {
            return false;
        }
        if (current_item->next)
        {
            *output_pointer++ = ',';
        }

        if (output_buffer->format)
        {
            *output_pointer++ = '\n';
        }
        *output_pointer = '\0';
        output_buffer->offset += length;

        current_item = current_item->next;
    }

    output_pointer = ensure(output_buffer, output_buffer->format ? (output_buffer->depth + 1) : 2);
    if (output_pointer == NULL)
    {
        return false;
    }
    if (output_buffer->format)
    {
        size_t i;
        for (i = 0; i < (output_buffer->depth - 1); i++)
        {
            *output_pointer++ = '\t';
        }
    }
    *output_pointer++ = '}';
    *output_pointer = '\0';
    output_buffer->depth--;

    return true;
}
/*
 * 函数名：cJSON_GetArraySize
 * 功能：获取JSON数组节点的元素个数（核心查询函数）
 *       遍历数组的子节点链表，统计元素数量，数组为NULL时返回0
 * 核心步骤：
 *   1. 参数校验：数组节点为NULL时直接返回0；
 *   2. 初始化遍历：获取数组的第一个子节点（child）；
 *   3. 链表遍历：循环遍历子节点链表，每遍历一个元素计数+1；
 *   4. 类型转换：将size_t类型的计数转换为int返回（存在溢出风险）；
 *   5. 返回结果：返回数组元素个数（数组非对象/数组类型时也返回0）。
 *
 * 参数：
 *   array - const cJSON*，待查询长度的JSON数组节点（可为NULL，非数组类型也视为无效）。
 *
 * 返回值：
 *   int，数组元素个数：
 *   - 0：array为NULL/array非数组类型/数组为空；
 *   - >0：数组的元素个数（存在溢出风险，见注意事项）。
 *
 * 注意事项：
 *   1. 链表遍历逻辑：cJSON数组底层是单向链表，通过child/next指针遍历所有元素；
 *   2. 溢出风险（FIXME）：size_t（通常64位系统是8字节）转int（4字节）时，若数组元素>2^31-1，会溢出导致返回负数/错误值，因API兼容性无法修复；
 *   3. 类型兼容：即使传入非数组节点（如对象/字符串），只要child链表不为空，也会返回链表长度（非预期行为，需确保传入数组节点）；
 *   4. 空数组处理：空数组（array不为NULL但child为NULL）返回0，符合预期；
 *   5. 性能：遍历整个链表，时间复杂度O(n)，n为数组元素个数。
 */
/* Get Array size/item / object item. */
CJSON_PUBLIC(int) cJSON_GetArraySize(const cJSON *array)
{
    cJSON *child = NULL;
    size_t size = 0;
    //step1:参数校验 - 如果传入的数组节点为NULL，直接返回0，表示无元素
    if (array == NULL)
    {
        return 0;
    }
    //step2:初始化遍历 - 获取数组的第一个子节点（child），准备开始遍历链表
    child = array->child;
    //step3:链表遍历 - 循环遍历子节点链表，每遍历一个元素计数+1，直到child为NULL表示链表末尾
    while(child != NULL)
    {
        size++;
        child = child->next;//移动到下一个节点
    }

    /* FIXME: Can overflow here. Cannot be fixed without breaking the API */
    //step4:类型转换 - 将size_t类型的计数转换为int返回，存在溢出风险（当元素个数超过2^31-1时）
    return (int)size;
}

static cJSON* get_array_item(const cJSON *array, size_t index)
{
    cJSON *current_child = NULL;

    if (array == NULL)
    {
        return NULL;
    }

    current_child = array->child;
    while ((current_child != NULL) && (index > 0))
    {
        index--;
        current_child = current_child->next;
    }

    return current_child;
}
/*
 * 函数名：cJSON_GetArrayItem
 * 功能：获取JSON数组指定索引的元素节点（核心查询函数）
 *       先校验索引合法性，再委托底层get_array_item遍历链表获取节点
 * 核心步骤：
 *   1. 索引合法性校验：索引<0时直接返回NULL（数组索引从0开始）；
 *   2. 类型转换：将int类型索引转为size_t（适配底层函数）；
 *   3. 委托底层函数：调用get_array_item遍历数组链表，获取指定索引的节点；
 *   4. 返回结果：找到则返回节点引用，未找到/参数非法则返回NULL。
 *
 * 参数：
 *   array - const cJSON*，待查询的JSON数组节点（可为NULL，非数组类型也视为无效）；
 *   index - int，目标元素的索引（从0开始计数，如0=第一个元素，1=第二个元素）。
 *
 * 返回值：
 *   成功 - cJSON*，指向数组指定索引元素的节点引用（不分配新内存，不可释放此指针）；
 *   失败 - NULL，原因：index<0/array为NULL/array非数组类型/索引越界（超过数组长度）。
 *
 * 注意事项：
 *   1. 索引规则：数组索引从0开始，最大有效索引为“数组长度-1”，越界返回NULL；
 *   2. 类型转换风险：int索引转size_t时，若index为负数（已提前校验）会变成超大正数，导致逻辑错误；
 *   3. 引用而非拷贝：返回的是原数组中节点的引用，修改此节点会直接改变原数组，无需释放此指针；
 *   4. 性能：遍历链表查找指定索引，时间复杂度O(n)，n为索引值（非数组总长度）；
 *   5. 空数组/非法节点：传入空数组/非数组节点时，底层函数会返回NULL，符合预期。
 */
CJSON_PUBLIC(cJSON *) cJSON_GetArrayItem(const cJSON *array, int index)
{
    //step1:索引合法性校验 - 如果索引小于0，直接返回NULL，表示无效索引
    if (index < 0)
    {
        return NULL;
    }
    //step2:类型转换 - 将int类型索引转为size_t，适配底层函数参数类型
    return get_array_item(array, (size_t)index);
}

static cJSON *get_object_item(const cJSON * const object, const char * const name, const cJSON_bool case_sensitive)
{
    cJSON *current_element = NULL;

    if ((object == NULL) || (name == NULL))
    {
        return NULL;
    }

    current_element = object->child;
    if (case_sensitive)
    {
        while ((current_element != NULL) && (current_element->string != NULL) && (strcmp(name, current_element->string) != 0))
        {
            current_element = current_element->next;
        }
    }
    else
    {
        while ((current_element != NULL) && (case_insensitive_strcmp((const unsigned char*)name, (const unsigned char*)(current_element->string)) != 0))
        {
            current_element = current_element->next;
        }
    }

    if ((current_element == NULL) || (current_element->string == NULL)) {
        return NULL;
    }

    return current_element;
}
/*
 * 函数名：cJSON_GetObjectItem
 * 功能：从JSON对象中获取指定键名的节点（核心查询函数）
 *       按键名精确匹配（区分大小写），底层委托get_object_item实现
 * 核心步骤：
 *   1. 委托底层函数：调用get_object_item，传入对象节点、目标键名、大小写敏感标识（false）；
 *   2. 返回结果：找到则返回对应节点指针，未找到/参数非法则返回NULL。
 *
 * 参数：
 *   object - const cJSON* const，待查询的JSON对象节点（需为cJSON_Object类型，不可修改，可为NULL）；
 *   string - const char* const，目标键名字符串（不可修改，不可为NULL，区分大小写）。
 *
 * 返回值：
 *   成功 - cJSON*，指向匹配键名的节点指针（仅返回引用，不分配新内存，不可释放此指针）；
 *   失败 - NULL，原因：object非对象类型/object为NULL/string为NULL/未找到匹配键名。
 *
 * 注意事项：
 *   1. 引用而非拷贝：返回的是原对象中节点的引用，修改此节点会直接改变原对象；
 *   2. 大小写敏感：键名匹配区分大小写（如"Name"≠"name"），若需忽略大小写可使用cJSON_GetObjectItemCaseSensitive；
 *   3. 内存安全：无需释放返回的节点指针（释放原object时会自动释放），否则会导致重复释放崩溃；
 *   4. 空值兼容：即使找到的节点值为NULL（如cJSON_NULL类型），也会返回节点指针（非NULL）；
 *   5. 性能：遍历对象链表查询键名，时间复杂度O(n)，n为对象的键值对数量。
 */
CJSON_PUBLIC(cJSON *) cJSON_GetObjectItem(const cJSON * const object, const char * const string)
{
    //委托底层get_object_item实现对象键名查询，大小写敏感标识为false
    return get_object_item(object, string, false);
}

CJSON_PUBLIC(cJSON *) cJSON_GetObjectItemCaseSensitive(const cJSON * const object, const char * const string)
{
    return get_object_item(object, string, true);
}

CJSON_PUBLIC(cJSON_bool) cJSON_HasObjectItem(const cJSON *object, const char *string)
{
    return cJSON_GetObjectItem(object, string) ? 1 : 0;
}

/* Utility for array list handling. */
static void suffix_object(cJSON *prev, cJSON *item)
{
    prev->next = item;
    item->prev = prev;
}

/* Utility for handling references. */
static cJSON *create_reference(const cJSON *item, const internal_hooks * const hooks)
{
    cJSON *reference = NULL;
    if (item == NULL)
    {
        return NULL;
    }

    reference = cJSON_New_Item(hooks);
    if (reference == NULL)
    {
        return NULL;
    }

    memcpy(reference, item, sizeof(cJSON));
    reference->string = NULL;
    reference->type |= cJSON_IsReference;
    reference->next = reference->prev = NULL;
    return reference;
}

static cJSON_bool add_item_to_array(cJSON *array, cJSON *item)
{
    cJSON *child = NULL;

    if ((item == NULL) || (array == NULL) || (array == item))
    {
        return false;
    }

    child = array->child;
    /*
     * To find the last item in array quickly, we use prev in array
     */
    if (child == NULL)
    {
        /* list is empty, start new one */
        array->child = item;
        item->prev = item;
        item->next = NULL;
    }
    else
    {
        /* append to the end */
        if (child->prev)
        {
            suffix_object(child->prev, item);
            array->child->prev = item;
        }
    }

    return true;
}

/* Add item to array/object. */
/*
 * 函数名：cJSON_AddItemToArray
 * 功能：向JSON数组节点添加元素（核心修改函数）
 *       是构建JSON数组的关键函数，底层委托add_item_to_array实现核心逻辑
 * 核心步骤：
 *   1. 委托底层函数：调用add_item_to_array，传入数组节点和待添加元素节点；
 *   2. 返回结果：透传底层函数的布尔返回值，标识元素是否添加成功。
 * 参数：
 *   array - cJSON*，待添加元素的JSON数组节点（需为cJSON_Array类型，不可为NULL）；
 *   item  - cJSON*，待添加的数组元素节点（不可为NULL，添加后所有权转移至array）。
 * 返回值：
 *   成功 - cJSON_True（1），元素已添加至数组节点末尾；
 *   失败 - cJSON_False（0），原因：参数非法/array非数组类型/内存操作失败。
 * 注意事项：
 *   1. 所有权转移：item节点添加后由array管理内存，无需单独释放，释放array时自动释放item；
 *   2. 数组特性：元素按添加顺序存储在数组末尾，支持重复元素、不同类型元素（如字符串/数字/对象）；
 *   3. 类型校验：array必须是cJSON_Array类型，否则添加失败（比如传入Object节点会返回False）；
 *   4. 空数组兼容：向空数组添加元素依然有效，数组会从空变为包含单个元素的状态；
 *   5. 重复添加：同一item节点不可多次添加到数组/对象，否则释放时会重复释放导致崩溃。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_AddItemToArray(cJSON *array, cJSON *item)
{
    //委托底层add_item_to_array实现数组元素添加逻辑
    return add_item_to_array(array, item);
}

#if defined(__clang__) || (defined(__GNUC__)  && ((__GNUC__ > 4) || ((__GNUC__ == 4) && (__GNUC_MINOR__ > 5))))
    #pragma GCC diagnostic push
#endif
#ifdef __GNUC__
#pragma GCC diagnostic ignored "-Wcast-qual"
#endif
/* helper function to cast away const */
static void* cast_away_const(const void* string)
{
    return (void*)string;
}
#if defined(__clang__) || (defined(__GNUC__)  && ((__GNUC__ > 4) || ((__GNUC__ == 4) && (__GNUC_MINOR__ > 5))))
    #pragma GCC diagnostic pop
#endif


static cJSON_bool add_item_to_object(cJSON * const object, const char * const string, cJSON * const item, const internal_hooks * const hooks, const cJSON_bool constant_key)
{
    char *new_key = NULL;
    int new_type = cJSON_Invalid;

    if ((object == NULL) || (string == NULL) || (item == NULL) || (object == item))
    {
        return false;
    }

    if (constant_key)
    {
        new_key = (char*)cast_away_const(string);
        new_type = item->type | cJSON_StringIsConst;
    }
    else
    {
        new_key = (char*)cJSON_strdup((const unsigned char*)string, hooks);
        if (new_key == NULL)
        {
            return false;
        }

        new_type = item->type & ~cJSON_StringIsConst;
    }

    if (!(item->type & cJSON_StringIsConst) && (item->string != NULL))
    {
        hooks->deallocate(item->string);
    }

    item->string = new_key;
    item->type = new_type;

    return add_item_to_array(object, item);
}
/*
 * 函数名：cJSON_AddItemToObject
 * 功能：向JSON对象节点添加键值对（核心修改函数）
 *       是构建JSON对象的关键函数，底层委托add_item_to_object实现核心逻辑
 * 核心步骤：
 *   1. 委托底层函数：调用add_item_to_object，传入固定参数完成键值对添加；
 *   2. 返回结果：透传底层函数的布尔返回值，标识添加是否成功。
 * 参数：
 *   object - cJSON*，待添加键值对的JSON对象节点（需为cJSON_Object类型，不可为NULL）；
 *   string - const char*，键名字符串（不可为NULL，需为合法UTF-8字符串）；
 *   item   - cJSON*，待添加的值节点（不可为NULL，添加后所有权转移至object）。
 * 返回值：
 *   成功 - cJSON_True（1），键值对已添加至对象节点；
 *   失败 - cJSON_False（0），原因：参数非法/object非对象类型/内存分配失败。
 * 注意事项：
 *   1. 所有权转移：item节点添加后，由object管理内存，无需单独释放item（释放object时自动释放）；
 *   2. 类型校验：object必须是cJSON_Object类型，否则添加失败；
 *   3. 钩子匹配：使用global_hooks完成键名字符串的内存分配，与全局内存管理逻辑一致；
 *   4. 重复添加：若object中已有相同键名，会覆盖原有节点（或导致重复键，取决于底层实现）。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_AddItemToObject(cJSON *object, const char *string, cJSON *item)
{
    //委托底层add_item_to_object实现：传入全局钩子+默认参数（false）
    return add_item_to_object(object, string, item, &global_hooks, false);
}

/* Add an item to an object with constant string as key */
//与cJSON_AddItemToObject的区别：后者会拷贝键名字符串，前者直接引用，更节省内存
CJSON_PUBLIC(cJSON_bool) cJSON_AddItemToObjectCS(cJSON *object, const char *string, cJSON *item)
{
    return add_item_to_object(object, string, item, &global_hooks, true);
}

CJSON_PUBLIC(cJSON_bool) cJSON_AddItemReferenceToArray(cJSON *array, cJSON *item)
{
    if (array == NULL)
    {
        return false;
    }

    return add_item_to_array(array, create_reference(item, &global_hooks));
}

CJSON_PUBLIC(cJSON_bool) cJSON_AddItemReferenceToObject(cJSON *object, const char *string, cJSON *item)
{
    if ((object == NULL) || (string == NULL))
    {
        return false;
    }

    return add_item_to_object(object, string, create_reference(item, &global_hooks), &global_hooks, false);
}

CJSON_PUBLIC(cJSON*) cJSON_AddNullToObject(cJSON * const object, const char * const name)
{
    cJSON *null = cJSON_CreateNull();
    if (add_item_to_object(object, name, null, &global_hooks, false))
    {
        return null;
    }

    cJSON_Delete(null);
    return NULL;
}

CJSON_PUBLIC(cJSON*) cJSON_AddTrueToObject(cJSON * const object, const char * const name)
{
    cJSON *true_item = cJSON_CreateTrue();
    if (add_item_to_object(object, name, true_item, &global_hooks, false))
    {
        return true_item;
    }

    cJSON_Delete(true_item);
    return NULL;
}

CJSON_PUBLIC(cJSON*) cJSON_AddFalseToObject(cJSON * const object, const char * const name)
{
    cJSON *false_item = cJSON_CreateFalse();
    if (add_item_to_object(object, name, false_item, &global_hooks, false))
    {
        return false_item;
    }

    cJSON_Delete(false_item);
    return NULL;
}

CJSON_PUBLIC(cJSON*) cJSON_AddBoolToObject(cJSON * const object, const char * const name, const cJSON_bool boolean)
{
    cJSON *bool_item = cJSON_CreateBool(boolean);
    if (add_item_to_object(object, name, bool_item, &global_hooks, false))
    {
        return bool_item;
    }

    cJSON_Delete(bool_item);
    return NULL;
}

CJSON_PUBLIC(cJSON*) cJSON_AddNumberToObject(cJSON * const object, const char * const name, const double number)
{
    cJSON *number_item = cJSON_CreateNumber(number);
    if (add_item_to_object(object, name, number_item, &global_hooks, false))
    {
        return number_item;
    }

    cJSON_Delete(number_item);
    return NULL;
}

CJSON_PUBLIC(cJSON*) cJSON_AddStringToObject(cJSON * const object, const char * const name, const char * const string)
{
    cJSON *string_item = cJSON_CreateString(string);
    if (add_item_to_object(object, name, string_item, &global_hooks, false))
    {
        return string_item;
    }

    cJSON_Delete(string_item);
    return NULL;
}

CJSON_PUBLIC(cJSON*) cJSON_AddRawToObject(cJSON * const object, const char * const name, const char * const raw)
{
    cJSON *raw_item = cJSON_CreateRaw(raw);
    if (add_item_to_object(object, name, raw_item, &global_hooks, false))
    {
        return raw_item;
    }

    cJSON_Delete(raw_item);
    return NULL;
}

CJSON_PUBLIC(cJSON*) cJSON_AddObjectToObject(cJSON * const object, const char * const name)
{
    cJSON *object_item = cJSON_CreateObject();
    if (add_item_to_object(object, name, object_item, &global_hooks, false))
    {
        return object_item;
    }

    cJSON_Delete(object_item);
    return NULL;
}

CJSON_PUBLIC(cJSON*) cJSON_AddArrayToObject(cJSON * const object, const char * const name)
{
    cJSON *array = cJSON_CreateArray();
    if (add_item_to_object(object, name, array, &global_hooks, false))
    {
        return array;
    }

    cJSON_Delete(array);
    return NULL;
}

CJSON_PUBLIC(cJSON *) cJSON_DetachItemViaPointer(cJSON *parent, cJSON * const item)
{
    if ((parent == NULL) || (item == NULL) || (item != parent->child && item->prev == NULL))
    {
        return NULL;
    }

    if (item != parent->child)
    {
        /* not the first element */
        item->prev->next = item->next;
    }
    if (item->next != NULL)
    {
        /* not the last element */
        item->next->prev = item->prev;
    }

    if (item == parent->child)
    {
        /* first element */
        parent->child = item->next;
    }
    else if (item->next == NULL)
    {
        /* last element */
        parent->child->prev = item->prev;
    }

    /* make sure the detached item doesn't point anywhere anymore */
    item->prev = NULL;
    item->next = NULL;

    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_DetachItemFromArray(cJSON *array, int which)
{
    if (which < 0)
    {
        return NULL;
    }

    return cJSON_DetachItemViaPointer(array, get_array_item(array, (size_t)which));
}
/*
 * 函数名：cJSON_DeleteItemFromArray
 * 功能：从JSON数组中删除指定索引的元素并释放其内存（数组核心修改函数）
 *       底层先分离索引对应的节点，再调用cJSON_Delete释放内存，是“分离+释放”的快捷操作
 * 核心步骤：
 *   1. 分离节点：调用cJSON_DetachItemFromArray从array中移除指定索引which的元素节点；
 *   2. 释放内存：调用cJSON_Delete释放分离出的节点（包括其所有子节点）；
 *   3. 无返回值：无论分离成功/失败，均执行释放（分离失败时传入NULL，cJSON_Delete会安全处理）。
 * 参数：
 *   array - cJSON*，待删除元素的JSON数组节点（需为cJSON_Array类型，可为NULL）；
 *   which - int，待删除元素的数组索引（从0开始计数；索引越界时分离返回NULL，无任何释放操作）。
 * 返回值：无（void类型）
 * 注意事项：
 *   1. 安全设计：array为NULL/索引越界时，cJSON_DetachItemFromArray返回NULL，cJSON_Delete(NULL)无操作，不会崩溃；
 *   2. 递归释放：删除的元素若为嵌套对象/数组，其所有子节点会被递归释放，无内存泄漏；
 *   3. 与Detach的区别：cJSON_DetachItemFromArray仅分离节点（不释放，可复用），本函数分离后直接释放，不可恢复；
 *   4. 索引规则：数组索引从0开始，如array[0]是第一个元素，索引越界时函数无任何效果；
 *   5. 数组结构：删除元素后，数组后续元素会自动前移，索引重新排列（如删除索引1，原索引2变为新索引1）。
 */
CJSON_PUBLIC(void) cJSON_DeleteItemFromArray(cJSON *array, int which)
{
    //分离节点并释放内存：先调用cJSON_DetachItemFromArray获取节点指针，再调用cJSON_Delete释放内存
    cJSON_Delete(cJSON_DetachItemFromArray(array, which));
}

CJSON_PUBLIC(cJSON *) cJSON_DetachItemFromObject(cJSON *object, const char *string)
{
    cJSON *to_detach = cJSON_GetObjectItem(object, string);

    return cJSON_DetachItemViaPointer(object, to_detach);
}

CJSON_PUBLIC(cJSON *) cJSON_DetachItemFromObjectCaseSensitive(cJSON *object, const char *string)
{
    cJSON *to_detach = cJSON_GetObjectItemCaseSensitive(object, string);

    return cJSON_DetachItemViaPointer(object, to_detach);
}
/*
 * 函数名：cJSON_DeleteItemFromObject
 * 功能：从JSON对象中删除指定键名的节点并释放其内存（核心修改函数）
 *       底层先分离节点，再调用cJSON_Delete释放内存，是“分离+释放”的快捷操作
 * 核心步骤：
 *   1. 分离节点：调用cJSON_DetachItemFromObject从object中移除指定键名的节点；
 *   2. 释放内存：调用cJSON_Delete释放分离出的节点（包括其所有子节点）；
 *   3. 无返回值：无论分离成功/失败，均执行释放（分离失败时传入NULL，cJSON_Delete会安全处理）。
 * 参数：
 *   object - cJSON*，待删除节点的JSON对象节点（需为cJSON_Object类型，可为NULL）；
 *   string - const char*，待删除节点的键名字符串（不可为NULL，需匹配对象中的键名）。
 * 返回值：无（void类型）
 * 注意事项：
 *   1. 安全设计：即使object为NULL/键名不存在（分离返回NULL），cJSON_Delete也不会崩溃；
 *   2. 递归释放：删除的节点若包含子节点（如嵌套对象/数组），会被递归释放，无内存泄漏；
 *   3. 与Detach的区别：Detach仅分离节点（不释放），本函数分离后直接释放，不可恢复；
 *   4. 键名匹配：字符串需与对象中的键名完全一致（区分大小写），否则无任何操作；
 *   5. 空对象兼容：向空对象调用此函数，仅会执行“分离（返回NULL）+释放（无操作）”，无异常。
 */
CJSON_PUBLIC(void) cJSON_DeleteItemFromObject(cJSON *object, const char *string)
{
    //分离节点并释放内存：先调用cJSON_DetachItemFromObject获取节点指针，再调用cJSON_Delete释放内存
    cJSON_Delete(cJSON_DetachItemFromObject(object, string));
}

CJSON_PUBLIC(void) cJSON_DeleteItemFromObjectCaseSensitive(cJSON *object, const char *string)
{
    cJSON_Delete(cJSON_DetachItemFromObjectCaseSensitive(object, string));
}

/* Replace array/object items with new ones. */
CJSON_PUBLIC(cJSON_bool) cJSON_InsertItemInArray(cJSON *array, int which, cJSON *newitem)
{
    cJSON *after_inserted = NULL;

    if (which < 0 || newitem == NULL)
    {
        return false;
    }

    after_inserted = get_array_item(array, (size_t)which);
    if (after_inserted == NULL)
    {
        return add_item_to_array(array, newitem);
    }

    if (after_inserted != array->child && after_inserted->prev == NULL) {
        /* return false if after_inserted is a corrupted array item */
        return false;
    }

    newitem->next = after_inserted;
    newitem->prev = after_inserted->prev;
    after_inserted->prev = newitem;
    if (after_inserted == array->child)
    {
        array->child = newitem;
    }
    else
    {
        newitem->prev->next = newitem;
    }
    return true;
}

CJSON_PUBLIC(cJSON_bool) cJSON_ReplaceItemViaPointer(cJSON * const parent, cJSON * const item, cJSON * replacement)
{
    if ((parent == NULL) || (parent->child == NULL) || (replacement == NULL) || (item == NULL))
    {
        return false;
    }

    if (replacement == item)
    {
        return true;
    }

    replacement->next = item->next;
    replacement->prev = item->prev;

    if (replacement->next != NULL)
    {
        replacement->next->prev = replacement;
    }
    if (parent->child == item)
    {
        if (parent->child->prev == parent->child)
        {
            replacement->prev = replacement;
        }
        parent->child = replacement;
    }
    else
    {   /*
         * To find the last item in array quickly, we use prev in array.
         * We can't modify the last item's next pointer where this item was the parent's child
         */
        if (replacement->prev != NULL)
        {
            replacement->prev->next = replacement;
        }
        if (replacement->next == NULL)
        {
            parent->child->prev = replacement;
        }
    }

    item->next = NULL;
    item->prev = NULL;
    cJSON_Delete(item);

    return true;
}

CJSON_PUBLIC(cJSON_bool) cJSON_ReplaceItemInArray(cJSON *array, int which, cJSON *newitem)
{
    if (which < 0)
    {
        return false;
    }

    return cJSON_ReplaceItemViaPointer(array, get_array_item(array, (size_t)which), newitem);
}

static cJSON_bool replace_item_in_object(cJSON *object, const char *string, cJSON *replacement, cJSON_bool case_sensitive)
{
    if ((replacement == NULL) || (string == NULL))
    {
        return false;
    }

    /* replace the name in the replacement */
    if (!(replacement->type & cJSON_StringIsConst) && (replacement->string != NULL))
    {
        cJSON_free(replacement->string);
    }
    replacement->string = (char*)cJSON_strdup((const unsigned char*)string, &global_hooks);
    if (replacement->string == NULL)
    {
        return false;
    }

    replacement->type &= ~cJSON_StringIsConst;

    return cJSON_ReplaceItemViaPointer(object, get_object_item(object, string, case_sensitive), replacement);
}

CJSON_PUBLIC(cJSON_bool) cJSON_ReplaceItemInObject(cJSON *object, const char *string, cJSON *newitem)
{
    return replace_item_in_object(object, string, newitem, false);
}

CJSON_PUBLIC(cJSON_bool) cJSON_ReplaceItemInObjectCaseSensitive(cJSON *object, const char *string, cJSON *newitem)
{
    return replace_item_in_object(object, string, newitem, true);
}

/* Create basic types: */
CJSON_PUBLIC(cJSON *) cJSON_CreateNull(void)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if(item)
    {
        item->type = cJSON_NULL;
    }

    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateTrue(void)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if(item)
    {
        item->type = cJSON_True;
    }

    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateFalse(void)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if(item)
    {
        item->type = cJSON_False;
    }

    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateBool(cJSON_bool boolean)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if(item)
    {
        item->type = boolean ? cJSON_True : cJSON_False;
    }

    return item;
}
/*
 * 函数名：cJSON_CreateNumber(类比cJSON_CreateString)
 * 功能：创建JSON数值类型节点（核心构建函数）
 *       支持double类型数值，同时处理int类型溢出，兼容valueint/valuedouble双字段存储
 * 核心步骤：
 *   1. 分配节点内存：调用cJSON_New_Item，传入全局钩子创建空cJSON节点；
 *   2. 设置节点类型：若节点创建成功，标记为cJSON_Number（数值类型）；
 *   3. 存储浮点值：将传入的double数值赋值给valuedouble字段；
 *   4. 溢出处理：将数值转换为int时做饱和处理，避免溢出导致的异常值；
 *   5. 存储整型值：将处理后的int值赋值给valueint字段；
 *   6. 返回结果：成功返回数值节点，失败返回NULL。
 * 参数：
 *   num - double，待封装为JSON数值的原始数值（支持整数、小数、正负值）
 * 返回值：
 *   成功 - cJSON*，指向JSON数值节点的指针；
 *   失败 - NULL，原因：节点内存分配失败（如堆空间不足）。
 * 注意事项：
 *   1. 双字段存储：节点同时保存valuedouble（原始浮点值）和valueint（整型近似值），兼顾精度和易用性；
 *   2. 溢出保护：数值超过INT_MAX/低于INT_MIN时，valueint会被设为边界值（饱和处理），避免溢出错误；
 *   3. 类型标识：节点type为cJSON_Number，可通过cJSON_IsNumber判断节点类型；
 *   4. 内存管理：创建的节点必须通过cJSON_Delete释放，无需单独释放数值字段（无额外内存分配）；
 *   5. 精度说明：valueint是valuedouble的整型截断值，小数部分会被舍弃（如3.9→3，-2.1→-2）。
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateNumber(double num)
{
    //step1:通过全局钩子创建空cJSON节点（分配内存+初始化）
    cJSON *item = cJSON_New_Item(&global_hooks);
    if(item)
    {
        //step2:设置节点类型为JSON数值
        item->type = cJSON_Number;
        //step3:将传入的double数值赋值给valuedouble字段
        item->valuedouble = num;

        /* use saturation in case of overflow */
        //step4:将数值转换为int时做饱和处理，避免溢出导致的异常值
        if (num >= INT_MAX)
        {
            //step5:若数值超过INT_MAX，将valueint设为INT_MAX    
            item->valueint = INT_MAX;
        }
        else if (num <= (double)INT_MIN)
        {
            //step5:若数值低于INT_MIN，将valueint设为INT_MIN
            item->valueint = INT_MIN;
        }
        else
        {
            //step5:数值在int范围内，直接转换为int赋值给valueint
            item->valueint = (int)num;
        }
    }

    return item;
}
/*
 * 函数名：cJSON_CreateString
 * 功能：创建JSON字符串类型节点（核心构建函数）
 *       自动拷贝传入的字符串到新内存，节点类型标记为cJSON_String
 * 核心步骤：
 *   1. 分配节点内存：调用cJSON_New_Item，传入全局钩子创建空cJSON节点；
 *   2. 设置节点类型：若节点创建成功，标记为cJSON_String（字符串类型）；
 *   3. 拷贝字符串：调用cJSON_strdup拷贝传入的string到新内存，赋值给valuestring；
 *   4. 失败回滚：若字符串拷贝失败，释放已创建的节点并返回NULL；
 *   5. 返回结果：成功返回字符串节点，失败返回NULL。
 * 参数：
 *   string - const char*，待封装为JSON字符串的原始字符串（可为NULL，NULL时创建空字符串节点）
 * 返回值：
 *   成功 - cJSON*，指向JSON字符串节点的指针；
 *   失败 - NULL，原因：节点内存分配失败/字符串拷贝失败（如内存不足）。
 * 注意事项：
 *   1. 字符串拷贝：函数会拷贝一份string到新内存，原字符串修改/释放不影响节点；
 *   2. 失败回滚：字符串拷贝失败时会自动释放节点，避免内存泄漏（核心健壮性设计）；
 *   3. 内存管理：创建的节点必须通过cJSON_Delete释放，释放时会自动释放valuestring；
 *   4. 空字符串兼容：传入NULL时，valuestring为NULL，节点仍为cJSON_String类型；
 *   5. 钩子匹配：节点和字符串内存均由global_hooks分配，释放时自动匹配。
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateString(const char *string)
{
    //step1:通过全局钩子创建空cJSON节点（分配内存+初始化）
    cJSON *item = cJSON_New_Item(&global_hooks);
    if(item)
    {
        //step2:设置节点类型为JSON字符串
        item->type = cJSON_String;
        //step3:拷贝传入字符串到新内存，赋值给valuestring
        item->valuestring = (char*)cJSON_strdup((const unsigned char*)string, &global_hooks);
        //step4:若字符串拷贝失败，释放已创建节点并返回NULL（核心健壮性设计）
        if(!item->valuestring)
        {
            cJSON_Delete(item);
            return NULL;
        }
    }

    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateStringReference(const char *string)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if (item != NULL)
    {
        item->type = cJSON_String | cJSON_IsReference;
        item->valuestring = (char*)cast_away_const(string);
    }

    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateObjectReference(const cJSON *child)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if (item != NULL) {
        item->type = cJSON_Object | cJSON_IsReference;
        item->child = (cJSON*)cast_away_const(child);
    }

    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateArrayReference(const cJSON *child) {
    cJSON *item = cJSON_New_Item(&global_hooks);
    if (item != NULL) {
        item->type = cJSON_Array | cJSON_IsReference;
        item->child = (cJSON*)cast_away_const(child);
    }

    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateRaw(const char *raw)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if(item)
    {
        item->type = cJSON_Raw;
        item->valuestring = (char*)cJSON_strdup((const unsigned char*)raw, &global_hooks);
        if(!item->valuestring)
        {
            cJSON_Delete(item);
            return NULL;
        }
    }

    return item;
}
/*
 * 函数名：cJSON_CreateArray
 * 功能：创建空的JSON数组节点（核心构建函数）
 *       是构建JSON数组的入口，创建后可通过cJSON_AddItemToArray添加数组元素
 * 核心步骤：
 *   1. 分配节点内存：调用cJSON_New_Item，传入全局内存钩子global_hooks创建空节点；
 *   2. 设置节点类型：若内存分配成功，将节点类型标记为cJSON_Array（JSON数组标识）；
 *   3. 返回结果：成功返回数组节点指针，失败返回NULL。
 * 参数：无
 * 返回值：
 *   成功 - cJSON*，指向空JSON数组节点的指针；
 *   失败 - NULL，原因：cJSON_New_Item内存分配失败（堆空间不足）。
 * 注意事项：
 *   1. 内存管理：创建的数组节点必须通过cJSON_Delete释放，否则造成内存泄漏；
 *   2. 空数组特征：初始无任何元素，需调用cJSON_AddItemToArray填充数组内容；
 *   3. 钩子匹配：节点由global_hooks分配内存，释放时会自动使用global_hooks.deallocate；
 *   4. 类型判断：可通过cJSON_IsArray(item)验证返回节点是否为JSON数组类型。
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateArray(void)
{
    //步骤1：通过全局钩子创建空cJSON节点（分配内存+初始化）
    cJSON *item = cJSON_New_Item(&global_hooks);
    if(item)
    {
        //步骤2：将节点类型设置为JSON数组（核心标识）
        item->type=cJSON_Array;
    }

    return item;
}
/*
 * 函数名：cJSON_CreateObject
 * 功能：创建空的JSON对象节点（核心构建函数）
 *       是构建JSON对象的入口，创建后可通过cJSON_AddItemToObject添加键值对
 * 核心步骤：
 *   1. 分配节点内存：调用cJSON_New_Item，传入全局内存钩子global_hooks创建空节点；
 *   2. 设置节点类型：若内存分配成功，将节点类型标记为cJSON_Object（JSON对象标识）；
 *   3. 返回结果：成功返回对象节点指针，失败返回NULL。
 * 参数：无
 * 返回值：
 *   成功 - cJSON*，指向空JSON对象节点的指针；
 *   失败 - NULL，原因：cJSON_New_Item内存分配失败（堆空间不足）。
 * 注意事项：
 *   1. 内存管理：创建的对象节点必须通过cJSON_Delete释放，否则造成内存泄漏；
 *   2. 空对象特征：初始无任何键值对，需调用cJSON_AddItemToObject填充内容；
 *   3. 钩子匹配：节点由global_hooks分配内存，释放时会自动使用global_hooks.deallocate；
 *   4. 类型判断：可通过cJSON_IsObject(item)验证返回节点是否为JSON对象类型。
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateObject(void)
{
    // 步骤1：通过全局钩子创建空cJSON节点（分配内存+初始化）
    cJSON *item = cJSON_New_Item(&global_hooks);
    if (item)
    {
       // 步骤2：将节点类型设置为JSON对象（核心标识） 
        item->type = cJSON_Object;
    }

    return item;
}

/* Create Arrays: */
CJSON_PUBLIC(cJSON *) cJSON_CreateIntArray(const int *numbers, int count)
{
    size_t i = 0;
    cJSON *n = NULL;
    cJSON *p = NULL;
    cJSON *a = NULL;

    if ((count < 0) || (numbers == NULL))
    {
        return NULL;
    }

    a = cJSON_CreateArray();

    for(i = 0; a && (i < (size_t)count); i++)
    {
        n = cJSON_CreateNumber(numbers[i]);
        if (!n)
        {
            cJSON_Delete(a);
            return NULL;
        }
        if(!i)
        {
            a->child = n;
        }
        else
        {
            suffix_object(p, n);
        }
        p = n;
    }

    if (a && a->child) {
        a->child->prev = n;
    }

    return a;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateFloatArray(const float *numbers, int count)
{
    size_t i = 0;
    cJSON *n = NULL;
    cJSON *p = NULL;
    cJSON *a = NULL;

    if ((count < 0) || (numbers == NULL))
    {
        return NULL;
    }

    a = cJSON_CreateArray();

    for(i = 0; a && (i < (size_t)count); i++)
    {
        n = cJSON_CreateNumber((double)numbers[i]);
        if(!n)
        {
            cJSON_Delete(a);
            return NULL;
        }
        if(!i)
        {
            a->child = n;
        }
        else
        {
            suffix_object(p, n);
        }
        p = n;
    }

    if (a && a->child) {
        a->child->prev = n;
    }

    return a;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateDoubleArray(const double *numbers, int count)
{
    size_t i = 0;
    cJSON *n = NULL;
    cJSON *p = NULL;
    cJSON *a = NULL;

    if ((count < 0) || (numbers == NULL))
    {
        return NULL;
    }

    a = cJSON_CreateArray();

    for(i = 0; a && (i < (size_t)count); i++)
    {
        n = cJSON_CreateNumber(numbers[i]);
        if(!n)
        {
            cJSON_Delete(a);
            return NULL;
        }
        if(!i)
        {
            a->child = n;
        }
        else
        {
            suffix_object(p, n);
        }
        p = n;
    }

    if (a && a->child) {
        a->child->prev = n;
    }

    return a;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateStringArray(const char *const *strings, int count)
{
    size_t i = 0;
    cJSON *n = NULL;
    cJSON *p = NULL;
    cJSON *a = NULL;

    if ((count < 0) || (strings == NULL))
    {
        return NULL;
    }

    a = cJSON_CreateArray();

    for (i = 0; a && (i < (size_t)count); i++)
    {
        n = cJSON_CreateString(strings[i]);
        if(!n)
        {
            cJSON_Delete(a);
            return NULL;
        }
        if(!i)
        {
            a->child = n;
        }
        else
        {
            suffix_object(p,n);
        }
        p = n;
    }

    if (a && a->child) {
        a->child->prev = n;
    }

    return a;
}
/*
 * 模块说明：Duplication（节点拷贝）- 提供cJSON节点的深拷贝功能，支持递归/非递归拷贝，防护循环引用
 */

/*
 * 函数声明：cJSON_Duplicate_rec
 * 功能：cJSON节点深拷贝递归实现函数（私有）- 核心深拷贝逻辑，带深度计数防止循环引用
 * 参数：
 *   item    - const cJSON*，待拷贝的源节点
 *   depth   - size_t，当前递归深度（用于防护循环引用）
 *   recurse - cJSON_bool，是否递归拷贝子节点（true=深拷贝，false=浅拷贝）
 * 返回值：cJSON* - 成功：拷贝后的新节点；失败：NULL
 */
cJSON * cJSON_Duplicate_rec(const cJSON *item, size_t depth, cJSON_bool recurse);

/*
 * 函数名：cJSON_Duplicate
 * 功能：cJSON节点深拷贝公共API - 对外暴露的拷贝接口，封装递归实现，初始深度为0
 * 核心设计：简化用户调用，无需关心递归深度参数，默认从深度0开始拷贝
 * 参数：
 *   item    - const cJSON*，待拷贝的源节点
 *   recurse - cJSON_bool，是否递归拷贝子节点（true=深拷贝所有子节点，false=仅拷贝当前节点）
 * 返回值：cJSON* - 成功：拷贝后的新节点；失败：NULL
 * 备注：CJSON_PUBLIC标记为公共API，供用户直接调用
 */
CJSON_PUBLIC(cJSON *) cJSON_Duplicate(const cJSON *item, cJSON_bool recurse)
{
    // 调用递归实现，初始深度0，传递是否递归的参数
    return cJSON_Duplicate_rec(item, 0, recurse );
}

/*
 * 函数名：cJSON_Duplicate_rec
 * 功能：cJSON节点深拷贝递归核心函数（私有）- 实现节点的完整深拷贝，包含值拷贝、字符串拷贝、子节点递归拷贝，防护循环引用
 * 核心设计思路：
 *       1. 分层拷贝：先拷贝当前节点的基础属性（类型/数值）→ 再拷贝字符串（深拷贝，避免指针引用）→ 最后递归拷贝子节点；
 *       2. 循环引用防护：通过depth限制递归深度（CJSON_CIRCULAR_LIMIT），防止循环引用导致栈溢出；
 *       3. 内存安全：字符串使用cJSON_strdup深拷贝（而非指针赋值），失败时自动释放已分配节点，无内存泄漏；
 *       4. 常量优化：字符串标记为cJSON_StringIsConst时，直接复用指针（无需拷贝，避免常量内存错误）；
 *       5. 链表重建：子节点通过next/prev重建双向链表，保证拷贝后的节点结构与源节点一致。
 * 参数：
 *   item    - const cJSON*，待拷贝的源节点
 *   depth   - size_t，当前递归深度（每次递归+1，超过阈值则失败）
 *   recurse - cJSON_bool，是否递归拷贝子节点（true=深拷贝，false=浅拷贝）
 * 返回值：cJSON* - 成功：拷贝后的新节点；失败：NULL
 */
cJSON * cJSON_Duplicate_rec(const cJSON *item, size_t depth, cJSON_bool recurse)
{
    cJSON *newitem = NULL;    // 拷贝后的新节点
    cJSON *child = NULL;      // 源节点的子节点遍历指针
    cJSON *next = NULL;       // 新节点的子节点链表尾指针（用于构建链表）
    cJSON *newchild = NULL;   // 拷贝后的子节点

    /* 第一步：空指针校验 - 源节点为NULL，直接跳转到失败逻辑 */
    if (!item)
    {
        goto fail;
    }

    /* 第二步：创建新节点 - 使用全局内存钩子分配内存，与源节点内存管理一致 */
    newitem = cJSON_New_Item(&global_hooks);
    if (!newitem)
    {
        goto fail; /* 内存分配失败，跳转失败逻辑 */
    }

    /* 第三步：拷贝基础属性 - 类型/数值（值拷贝，无指针引用）
     * ~cJSON_IsReference：清除引用标记，新节点为独立节点，非引用
     */
    newitem->type = item->type & (~cJSON_IsReference);
    newitem->valueint = item->valueint;          // 拷贝整型值
    newitem->valuedouble = item->valuedouble;    // 拷贝浮点值

    /* 第四步：拷贝valuestring（字符串类型节点的值）- 深拷贝，避免指针共享 */
    if (item->valuestring)
    {
        // cJSON_strdup：深拷贝字符串，使用全局内存钩子分配内存
        newitem->valuestring = (char*)cJSON_strdup((unsigned char*)item->valuestring, &global_hooks);
        if (!newitem->valuestring)
        {
            goto fail; /* 字符串拷贝失败，跳转失败逻辑 */
        }
    }

    /* 第五步：拷贝string（对象节点的键名）- 区分常量/普通字符串优化拷贝
     * 优化逻辑：
     *   - 标记为cJSON_StringIsConst：键名为常量字符串，直接复用指针（避免拷贝常量内存）；
     *   - 普通字符串：调用cJSON_strdup深拷贝，保证独立内存；
     */
    if (item->string)
    {
        newitem->string = (item->type&cJSON_StringIsConst) ? item->string : (char*)cJSON_strdup((unsigned char*)item->string, &global_hooks);
        if (!newitem->string)
        {
            goto fail; /* 键名拷贝失败，跳转失败逻辑 */
        }
    }

    /* 第六步：非递归拷贝 - 仅拷贝当前节点，无需处理子节点，直接返回新节点 */
    if (!recurse)
    {
        return newitem;
    }

    /* 第七步：递归拷贝子节点（深拷贝核心）
     * 遍历源节点的子节点链表，逐个递归拷贝，重建双向链表
     */
    child = item->child; // 指向源节点的第一个子节点
    while (child != NULL)
    {
        /* 7.1 循环引用防护 - 递归深度超过阈值（CJSON_CIRCULAR_LIMIT），防止栈溢出 */
        if(depth >= CJSON_CIRCULAR_LIMIT) {
            goto fail;
        }

        /* 7.2 递归拷贝当前子节点 - 深度+1，强制递归（保证子节点的深拷贝） */
        newchild = cJSON_Duplicate_rec(child, depth + 1, true);
        if (!newchild)
        {
            goto fail; /* 子节点拷贝失败，跳转失败逻辑 */
        }

        /* 7.3 构建子节点双向链表 */
        if (next != NULL)
        {
            /* 场景1：已有子节点 - 将新子节点挂到链表尾，维护prev/next指针 */
            next->next = newchild;
            newchild->prev = next;
            next = newchild; // 更新链表尾指针
        }
        else
        {
            /* 场景2：第一个子节点 - 设为新节点的child，初始化链表尾指针 */
            newitem->child = newchild;
            next = newchild;
        }

        child = child->next; // 遍历源节点的下一个子节点
    }

    /* 7.4 链表闭环处理 - 新节点的第一个子节点的prev指向最后一个子节点（cJSON特有设计，便于反向遍历） */
    if (newitem && newitem->child)
    {
        newitem->child->prev = newchild;
    }

    // 拷贝完成，返回新节点
    return newitem;

/* 失败处理逻辑：统一释放已分配的新节点，避免内存泄漏 */
fail:
    if (newitem != NULL)
    {
        cJSON_Delete(newitem); // 递归释放新节点及其所有已拷贝的子节点
    }

    return NULL; // 拷贝失败，返回NULL
}
static void skip_oneline_comment(char **input)
{
    *input += static_strlen("//");

    for (; (*input)[0] != '\0'; ++(*input))
    {
        if ((*input)[0] == '\n') {
            *input += static_strlen("\n");
            return;
        }
    }
}

static void skip_multiline_comment(char **input)
{
    *input += static_strlen("/*");

    for (; (*input)[0] != '\0'; ++(*input))
    {
        if (((*input)[0] == '*') && ((*input)[1] == '/'))
        {
            *input += static_strlen("*/");
            return;
        }
    }
}

static void minify_string(char **input, char **output) {
    (*output)[0] = (*input)[0];
    *input += static_strlen("\"");
    *output += static_strlen("\"");


    for (; (*input)[0] != '\0'; (void)++(*input), ++(*output)) {
        (*output)[0] = (*input)[0];

        if ((*input)[0] == '\"') {
            (*output)[0] = '\"';
            *input += static_strlen("\"");
            *output += static_strlen("\"");
            return;
        } else if (((*input)[0] == '\\') && ((*input)[1] == '\"')) {
            (*output)[1] = (*input)[1];
            *input += static_strlen("\"");
            *output += static_strlen("\"");
        }
    }
}

CJSON_PUBLIC(void) cJSON_Minify(char *json)
{
    char *into = json;

    if (json == NULL)
    {
        return;
    }

    while (json[0] != '\0')
    {
        switch (json[0])
        {
            case ' ':
            case '\t':
            case '\r':
            case '\n':
                json++;
                break;

            case '/':
                if (json[1] == '/')
                {
                    skip_oneline_comment(&json);
                }
                else if (json[1] == '*')
                {
                    skip_multiline_comment(&json);
                } else {
                    json++;
                }
                break;

            case '\"':
                minify_string(&json, (char**)&into);
                break;

            default:
                into[0] = json[0];
                json++;
                into++;
        }
    }

    /* and null-terminate. */
    *into = '\0';
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsInvalid(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xFF) == cJSON_Invalid;
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsFalse(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xFF) == cJSON_False;
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsTrue(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xff) == cJSON_True;
}


CJSON_PUBLIC(cJSON_bool) cJSON_IsBool(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & (cJSON_True | cJSON_False)) != 0;
}
CJSON_PUBLIC(cJSON_bool) cJSON_IsNull(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xFF) == cJSON_NULL;
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsNumber(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xFF) == cJSON_Number;
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsString(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xFF) == cJSON_String;
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsArray(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xFF) == cJSON_Array;
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsObject(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xFF) == cJSON_Object;
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsRaw(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xFF) == cJSON_Raw;
}

CJSON_PUBLIC(cJSON_bool) cJSON_Compare(const cJSON * const a, const cJSON * const b, const cJSON_bool case_sensitive)
{
    if ((a == NULL) || (b == NULL) || ((a->type & 0xFF) != (b->type & 0xFF)))
    {
        return false;
    }

    /* check if type is valid */
    switch (a->type & 0xFF)
    {
        case cJSON_False:
        case cJSON_True:
        case cJSON_NULL:
        case cJSON_Number:
        case cJSON_String:
        case cJSON_Raw:
        case cJSON_Array:
        case cJSON_Object:
            break;

        default:
            return false;
    }

    /* identical objects are equal */
    if (a == b)
    {
        return true;
    }

    switch (a->type & 0xFF)
    {
        /* in these cases and equal type is enough */
        case cJSON_False:
        case cJSON_True:
        case cJSON_NULL:
            return true;

        case cJSON_Number:
            if (compare_double(a->valuedouble, b->valuedouble))
            {
                return true;
            }
            return false;

        case cJSON_String:
        case cJSON_Raw:
            if ((a->valuestring == NULL) || (b->valuestring == NULL))
            {
                return false;
            }
            if (strcmp(a->valuestring, b->valuestring) == 0)
            {
                return true;
            }

            return false;

        case cJSON_Array:
        {
            cJSON *a_element = a->child;
            cJSON *b_element = b->child;

            for (; (a_element != NULL) && (b_element != NULL);)
            {
                if (!cJSON_Compare(a_element, b_element, case_sensitive))
                {
                    return false;
                }

                a_element = a_element->next;
                b_element = b_element->next;
            }

            /* one of the arrays is longer than the other */
            if (a_element != b_element) {
                return false;
            }

            return true;
        }

        case cJSON_Object:
        {
            cJSON *a_element = NULL;
            cJSON *b_element = NULL;
            cJSON_ArrayForEach(a_element, a)
            {
                /* TODO This has O(n^2) runtime, which is horrible! */
                b_element = get_object_item(b, a_element->string, case_sensitive);
                if (b_element == NULL)
                {
                    return false;
                }

                if (!cJSON_Compare(a_element, b_element, case_sensitive))
                {
                    return false;
                }
            }

            /* doing this twice, once on a and b to prevent true comparison if a subset of b
             * TODO: Do this the proper way, this is just a fix for now */
            cJSON_ArrayForEach(b_element, b)
            {
                a_element = get_object_item(a, b_element->string, case_sensitive);
                if (a_element == NULL)
                {
                    return false;
                }

                if (!cJSON_Compare(b_element, a_element, case_sensitive))
                {
                    return false;
                }
            }

            return true;
        }

        default:
            return false;
    }
}
/*
 * 函数名：cJSON_malloc
 * 功能：cJSON库封装的内存分配函数，简化malloc调用，统一内存分配入口
 *       替代直接调用系统malloc，便于后续替换内存分配器
 * 核心步骤：
 *   1. 系统调用：直接调用标准库malloc函数分配指定大小的内存；
 *   2. 返回结果：透传malloc的返回值（成功返回指针，失败返回NULL）。
 * 参数：
 *   size - size_t，待分配的内存字节数（可为0，0时malloc行为依赖系统）
 * 返回值：
 *   成功 - void*，指向分配内存的起始地址指针；
 *   失败 - NULL，原因：内存不足/分配大小非法。
 * 注意事项：
 *   1. 统一入口：所有cJSON内部内存分配均调用此函数，便于全局管理；
 *   2. 匹配原则：分配的内存必须通过cJSON_free释放，避免内存泄漏；
 *   3. 未初始化：分配的内存含随机值，需手动初始化（如memset）；
 *   4. 大小校验：建议调用前校验size是否合理，避免分配超大内存导致失败。
 */
CJSON_PUBLIC(void *) cJSON_malloc(size_t size)
{
    //调用系统malloc分配内存，统一内存分配入口
    return global_hooks.allocate(size);
}
/*
 * 函数名：cJSON_free
 * 功能：cJSON库封装的内存释放函数，基于全局内存钩子释放内存并置空指针
 *       是cJSON内存管理的统一出口，替代直接调用系统free，提升安全性
 * 核心步骤：
 *   1. 释放内存：通过全局内存钩子（global_hooks）的deallocate函数释放传入的内存；
 *   2. 指针置空：主动将传入的指针置为NULL，彻底避免野指针访问风险；
 *   3. 无返回值：仅执行释放+置空操作，不返回任何状态。
 * 参数：
 *   object - void*，待释放的内存指针（可为NULL，NULL时仍会执行置空操作）
 * 返回值：无（void类型）
 * 注意事项：
 *   1. 全局钩子依赖：释放逻辑由global_hooks.deallocate决定（默认绑定系统free函数）；
 *   2. 指针置空：函数内将参数置NULL仅修改形参，需注意：调用者需自行置空实参指针；
 *   3. 匹配原则：释放的内存必须是通过cJSON_malloc/global_hooks.allocate分配的，否则触发内存错误；
 *   4. 空指针处理：传入NULL时，deallocate会安全处理（无崩溃），且会将object置为NULL。
 */
CJSON_PUBLIC(void) cJSON_free(void *object)
{
    //通过全局内存钩子释放内存（统一内存释放入口）
    global_hooks.deallocate(object);
    //置空指针：避免后续误访问已释放的内存（仅修改函数内的形参）
    object = NULL;
}
/* ===================== 优化输出模块（追加到cJSON.c末尾） ===================== */
/* 格式化打印JSON（带缩进、换行，人类可读） */
static void print_indent(printbuffer *p, int depth)
{
    unsigned char *buf = ensure(p, (size_t)(depth * 4));
    if (buf == NULL) return;
    
    for (int i = 0; i < depth; i++) {
        memcpy(buf + i*4, "    ", 4);
    }
    p->offset += depth * 4;
}

static cJSON_bool print_value(const cJSON *item, printbuffer *p, int depth);

/* 打印JSON键值对（带格式化） */
static cJSON_bool print_pair(const cJSON *item, printbuffer *p, int depth)
{
    unsigned char *buf = NULL;
    size_t len = 0;

    /* 打印缩进 */
    print_indent(p, depth);
    if (p->buffer == NULL) return false;

    /* 打印键名 */
    len = strlen(item->string) + 2; // 包含双引号
    buf = ensure(p, len);
    if (buf == NULL) return false;
    sprintf((char*)buf, "\"%s\"", item->string);
    p->offset += len - 1; // 减去sprintf自动加的\0

    /* 打印冒号+空格 */
    buf = ensure(p, 2);
    if (buf == NULL) return false;
    memcpy(buf, ": ", 2);
    p->offset += 2;

    /* 打印值 */
    return print_value(item, p, depth);
}

/* 打印JSON值（递归处理对象/数组） */
static cJSON_bool print_value(const cJSON *item, printbuffer *p, int depth)
{
    if (item == NULL || p == NULL) return false;
    unsigned char *buf = NULL;

    switch (item->type & 0xFF) {
        case cJSON_String:
            len = strlen(item->valuestring) + 2;
            buf = ensure(p, len);
            if (buf == NULL) return false;
            sprintf((char*)buf, "\"%s\"", item->valuestring);
            p->offset += len - 1;
            break;

        case cJSON_Number:
            return print_number(item, p);

        case cJSON_Array: {
            buf = ensure(p, 2); // [ + 换行
            if (buf == NULL) return false;
            memcpy(buf, "[\n", 2);
            p->offset += 2;

            cJSON *child = item->child;
            while (child != NULL) {
                print_pair(child, p, depth + 1);
                if (child->next != NULL) {
                    buf = ensure(p, 2); // , + 换行
                    if (buf == NULL) return false;
                    memcpy(buf, ",\n", 2);
                    p->offset += 2;
                }
                child = child->next;
            }

            print_indent(p, depth);
            buf = ensure(p, 2); // ] + 换行
            if (buf == NULL) return false;
            memcpy(buf, "]\n", 2);
            p->offset += 2;
            break;
        }

        default:
            buf = ensure(p, 5);
            if (buf == NULL) return false;
            memcpy(buf, "null\n", 5);
            p->offset += 5;
            break;
    }

    return true;
}

/* 公共API：格式化输出JSON字符串 */
CJSON_PUBLIC(char *) cJSON_PrintPretty(const cJSON *item)
{
    printbuffer p = {0};
    p.hooks = global_hooks;
    p.buffer = (unsigned char*)p.hooks.allocate(256); // 初始缓冲区
    p.length = 256;
    p.format = true;

    if (print_value(item, &p, 0) == false) {
        p.hooks.deallocate(p.buffer);
        return NULL;
    }

    /* 截断多余空间 */
    unsigned char *result = (unsigned char*)p.hooks.allocate(p.offset + 1);
    memcpy(result, p.buffer, p.offset);
    result[p.offset] = '\0';
    p.hooks.deallocate(p.buffer);

    return (char*)result;
}

/* 公共API：压缩输出JSON（无空格、换行，节省空间） */
CJSON_PUBLIC(char *) cJSON_PrintCompress(const cJSON *item)
{
    printbuffer p = {0};
    p.hooks = global_hooks;
    p.buffer = (unsigned char*)p.hooks.allocate(256);
    p.length = 256;
    p.format = false;

    if (print_value(item, &p, 0) == false) {
        p.hooks.deallocate(p.buffer);
        return NULL;
    }

    unsigned char *result = (unsigned char*)p.hooks.allocate(p.offset + 1);
    memcpy(result, p.buffer, p.offset);
    result[p.offset] = '\0';
    p.hooks.deallocate(p.buffer);

    return (char*)result;
}

/* 调试用：带颜色输出JSON（终端友好） */
CJSON_PUBLIC(void) cJSON_PrintColor(const cJSON *item)
{
    if (item == NULL) return;
    char *pretty = cJSON_PrintPretty(item);
    if (pretty == NULL) return;

    /* 简单颜色替换：字符串标红，数字标绿，关键字标蓝 */
    for (char *p = pretty; *p != '\0'; p++) {
        if (*p == '"' && *(p+1) != '\0') {
            printf("\033[31m"); // 红色
            putchar(*p); p++;
            while (*p != '"' && *p != '\0') { putchar(*p); p++; }
            putchar(*p);
            printf("\033[0m"); // 重置
        }
        else if ((*p >= '0' && *p <= '9') || *p == '-' || *p == '.') {
            printf("\033[32m"); // 绿色
            while (((*p >= '0' && *p <= '9') || *p == '-' || *p == '.' || *p == 'e' || *p == 'E') && *p != '\0') {
                putchar(*p); p++;
            }
            p--;
            printf("\033[0m");
        }
        else if (strncmp(p, "null", 4) == 0 || strncmp(p, "true", 4) == 0 || strncmp(p, "false", 5) == 0) {
            printf("\033[34m"); // 蓝色
            if (strncmp(p, "null", 4) == 0) { printf("null"); p += 3; }
            else if (strncmp(p, "true", 4) == 0) { printf("true"); p += 3; }
            else if (strncmp(p, "false", 5) == 0) { printf("false"); p += 4; }
            printf("\033[0m");
        }
        else {
            putchar(*p);
        }
    }
    printf("\n");
    cJSON_free(pretty);
}

/* 简化API：直接打印JSON（自动选择格式化/压缩） */
CJSON_PUBLIC(void) cJSON_Print(const cJSON *item, cJSON_bool pretty)
{
    if (item == NULL) return;
    char *out = pretty ? cJSON_PrintPretty(item) : cJSON_PrintCompress(item);
    if (out != NULL) {
        printf("%s\n", out);
        cJSON_free(out);
    }
}
/* ===================== 优化输出模块 结束 ===================== */
