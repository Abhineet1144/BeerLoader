/* OLEAUT32 VARIANTs. VARIANT (x64, 24 bytes): vt u16 +0, value +8. Conversions cover VT_EMPTY/NULL, the integer
 * types, VT_R4/R8/DATE, VT_BOOL and VT_BSTR (formatted and parsed in the C locale). Interface-typed and other variants
 * report DISP_E_BADVARTYPE / DISP_E_TYPEMISMATCH rather than a made-up value. */
#include "win32_variant.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DISP_E_BADVARTYPE   0x80020008u
#define DISP_E_TYPEMISMATCH 0x80020005u
#define E_INVALIDARG        0x80070057u

static BeerBstrAlloc g_bstr_alloc;
static BeerBstrFree g_bstr_free;

void beer_variant_set_bstr_allocator(BeerBstrAlloc alloc, BeerBstrFree release)
{
    g_bstr_alloc = alloc;
    g_bstr_free = release;
}

static uint64_t bstr_copy(const uint16_t *text)
{
    if (!text || !g_bstr_alloc) return 0;
    return g_bstr_alloc(text, ((const uint32_t *)text)[-1] / 2);   /* the BSTR byte length precedes the text */
}

uint64_t __attribute__((ms_abi)) beer_VariantInit(uint8_t *variant)
{
    if (variant) memset(variant, 0, 24);
    return 0;
}

uint64_t __attribute__((ms_abi)) beer_VariantClear(uint8_t *variant)
{
    if (!variant) return E_INVALIDARG;
    if (*(uint16_t *)variant == 8 && *(uint16_t **)(variant + 8) && g_bstr_free)
        g_bstr_free(*(uint16_t **)(variant + 8));
    memset(variant, 0, 24);
    return 0;
}

uint64_t __attribute__((ms_abi)) beer_VariantCopy(uint8_t *destination, const uint8_t *source)
{
    if (!destination || !source) return E_INVALIDARG;
    if (destination == source) return 0;
    beer_VariantClear(destination);
    memcpy(destination, source, 24);
    if (*(const uint16_t *)source == 8)
        *(uint64_t *)(destination + 8) = bstr_copy(*(const uint16_t *const *)(source + 8));
    return 0;
}

/* The numeric value of a variant; 0 when it has none (or a BSTR that is not a number). */
static int variant_number(const uint8_t *v, double *real, int64_t *integer, int *is_real)
{
    *is_real = 0;
    *real = 0;
    *integer = 0;
    switch (*(const uint16_t *)v) {
    case 0: case 1: return 1;                                                   /* VT_EMPTY, VT_NULL */
    case 16: *integer = *(const int8_t *)(v + 8); return 1;                     /* VT_I1 */
    case 17: *integer = *(const uint8_t *)(v + 8); return 1;                    /* VT_UI1 */
    case 2: *integer = *(const int16_t *)(v + 8); return 1;                     /* VT_I2 */
    case 18: *integer = *(const uint16_t *)(v + 8); return 1;                   /* VT_UI2 */
    case 3: case 22: case 10: *integer = *(const int32_t *)(v + 8); return 1;   /* VT_I4, VT_INT, VT_ERROR */
    case 19: case 23: *integer = *(const uint32_t *)(v + 8); return 1;          /* VT_UI4, VT_UINT */
    case 20: case 21: *integer = *(const int64_t *)(v + 8); return 1;           /* VT_I8, VT_UI8 */
    case 11: *integer = *(const int16_t *)(v + 8) ? -1 : 0; return 1;           /* VT_BOOL */
    case 4: *real = *(const float *)(v + 8); *is_real = 1; return 1;            /* VT_R4 */
    case 5: case 7: *real = *(const double *)(v + 8); *is_real = 1; return 1;   /* VT_R8, VT_DATE */
    case 8: {                                                                   /* VT_BSTR */
        const uint16_t *text = *(const uint16_t *const *)(v + 8);
        char narrow[128];
        size_t n = 0;
        while (text && text[n] && n + 1 < sizeof(narrow)) { narrow[n] = (char)(text[n] < 0x80 ? text[n] : '?'); ++n; }
        narrow[n] = 0;
        if (!strcmp(narrow, "True")) { *integer = -1; return 1; }
        if (!strcmp(narrow, "False")) return 1;
        char *end = NULL;
        *real = strtod(narrow, &end);
        if (end == narrow || *end) return 0;
        *is_real = strpbrk(narrow, ".eE") != NULL;
        *integer = (int64_t)*real;
        return 1;
    }
    default:
        return 0;
    }
}

uint64_t __attribute__((ms_abi)) beer_VariantChangeTypeEx(uint8_t *destination, const uint8_t *source, uint32_t locale,
                                                          uint16_t flags, uint16_t vt)
{
    (void)locale; (void)flags;
    if (!destination || !source) return E_INVALIDARG;
    if (*(const uint16_t *)source == vt) return beer_VariantCopy(destination, source);
    double real;
    int64_t integer;
    int is_real;
    if (!variant_number(source, &real, &integer, &is_real)) return DISP_E_TYPEMISMATCH;
    uint8_t result[24] = {0};
    *(uint16_t *)result = vt;
    if (vt == 8) {                                                              /* to VT_BSTR */
        char text[64];
        if (*(const uint16_t *)source == 11) snprintf(text, sizeof(text), "%s", integer ? "True" : "False");
        else if (is_real) snprintf(text, sizeof(text), "%.15g", real);
        else snprintf(text, sizeof(text), "%lld", (long long)integer);
        uint16_t wide[64];
        size_t n = 0;
        for (; text[n]; ++n) wide[n] = (uint8_t)text[n];
        wide[n] = 0;
        *(uint64_t *)(result + 8) = g_bstr_alloc ? g_bstr_alloc(wide, (uint32_t)n) : 0;
    } else {
        if (is_real) integer = (int64_t)(real < 0 ? real - 0.5 : real + 0.5);
        switch (vt) {
        case 0: case 1: break;
        case 16: case 17: *(uint8_t *)(result + 8) = (uint8_t)integer; break;
        case 2: case 18: *(uint16_t *)(result + 8) = (uint16_t)integer; break;
        case 3: case 19: case 22: case 23: case 10: *(uint32_t *)(result + 8) = (uint32_t)integer; break;
        case 20: case 21: *(int64_t *)(result + 8) = integer; break;
        case 11: *(int16_t *)(result + 8) = (is_real ? real != 0 : integer != 0) ? -1 : 0; break;
        case 4: *(float *)(result + 8) = is_real ? (float)real : (float)integer; break;
        case 5: case 7: *(double *)(result + 8) = is_real ? real : (double)integer; break;
        default: return DISP_E_BADVARTYPE;
        }
    }
    beer_VariantClear(destination);   /* also correct when destination == source: the value was read above */
    memcpy(destination, result, 24);
    return 0;
}

uint64_t __attribute__((ms_abi)) beer_VariantChangeType(uint8_t *destination, const uint8_t *source, uint16_t flags,
                                                        uint16_t vt)
{
    return beer_VariantChangeTypeEx(destination, source, 0x409, flags, vt);
}
