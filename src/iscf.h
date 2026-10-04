/* iscf.h — формат ISCF v1 (.isc, Its So Cool Format) */
#ifndef ISCF_H
#define ISCF_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t  i32;

#define ISCF_MAGIC      "ISCF"
#define ISCF_VERSION    1

/* заголовок архива, 32 байта, little-endian */
#define HDR_SIZE        32
/* подвал, 16 байт */
#define FTR_SIZE        16

/* рецепты блока — решает маршрутизатор */
#define R_STORE 0       /* уже сжатые / мелкие / несжимаемые */
#define R_RLE   1       /* повторяющиеся данные */
#define R_LZT   2       /* текстовый пайплайн LZI */
#define R_LZB   3       /* бинарный пайплайн LZI */
#define R_DLT   4       /* дельта-фильтр + LZI: [шаг 1|2|4|8][поток LZI] */

/* типы записей */
#define T_FILE 0
#define T_DIR  1
#define T_LINK 2        /* дедуп: ссылка на другую запись */

#define BLOCK_RAW   (1u << 20)          /* 1 МиБ сырого блока */
#define MAX_MATCH   347
#define MIN_MATCH   4
#define WIN_RAW     BLOCK_RAW           /* окно = блок, блоки независимы */

/* LZI: алфавиты */
#define LEN_SLOTS   34                  /* длины 4..347 */
#define DST_SLOTS   40                  /* дистанции 1..2^20 */
#define LIT_SYMS    256
#define EOB         256
#define LITN_SYMS   (LIT_SYMS + 1 + LEN_SLOTS)  /* 291 */
#define HLIM        15                  /* предел длины кода Хаффмана */

/* le-кодирование (формат определён в little-endian) */
static inline u16 le16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static inline u32 le32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static inline u64 le64(const u8 *p) { return (u64)le32(p) | (u64)le32(p + 4) << 32; }
static inline void st16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static inline void st32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static inline void st64(u8 *p, u64 v) { st32(p, (u32)v); st32(p + 4, (u32)(v >> 32)); }

#endif
