#ifndef BEER_WIN32_FILESYSTEM_POLICY_H
#define BEER_WIN32_FILESYSTEM_POLICY_H

#include <stdint.h>
#include <string.h>

/* GUID bytes use the in-memory Win32 layout: Data1/Data2/Data3 are little
 * endian, while Data4 is stored in declaration order. */
static inline int beer_guid_equal(const void *value, const uint8_t expected[16])
{
    return value && memcmp(value, expected, 16) == 0;
}

typedef enum BeerKnownFolder {
    BEER_KNOWN_FOLDER_UNKNOWN = 0,
    BEER_KNOWN_FOLDER_PROFILE,
    BEER_KNOWN_FOLDER_DOCUMENTS,
    BEER_KNOWN_FOLDER_SAVED_GAMES,
    BEER_KNOWN_FOLDER_ROAMING_APPDATA,
    BEER_KNOWN_FOLDER_LOCAL_APPDATA,
    BEER_KNOWN_FOLDER_PROGRAM_DATA
} BeerKnownFolder;

static inline BeerKnownFolder beer_classify_known_folder(const void *rfid)
{
    static const uint8_t profile[16] =
        {0x8f,0x85,0x6c,0x5e,0x20,0xe1,0x41,0x42,0xa7,0xea,0x3f,0xd6,0xd0,0xff,0x5f,0x20};
    static const uint8_t documents[16] =
        {0xd0,0x9a,0xd3,0xfd,0x8f,0x23,0xaf,0x46,0xad,0xb4,0x6c,0x85,0x48,0x03,0x69,0xc7};
    static const uint8_t saved_games[16] =
        {0xff,0x32,0x5c,0x4c,0x9d,0xbb,0xb0,0x43,0xb5,0xb4,0x2d,0x72,0xe5,0x4e,0xaa,0xa4};
    static const uint8_t roaming[16] =
        {0xdb,0x85,0xb6,0x3e,0x6d,0x65,0x48,0x4c,0xba,0xc1,0x3f,0x80,0x03,0x4d,0x20,0x7e};
    static const uint8_t local[16] =
        {0x85,0x27,0xb3,0xf1,0x72,0x6f,0x4a,0x40,0x8f,0x95,0x4e,0x6c,0x3b,0xfa,0x12,0x4a};
    static const uint8_t program_data[16] =
        {0x82,0x5d,0xab,0x62,0xc1,0xfd,0xdc,0x4d,0xa9,0xdd,0x07,0x0d,0x1d,0x49,0x5d,0x97};

    if (beer_guid_equal(rfid, profile)) return BEER_KNOWN_FOLDER_PROFILE;
    if (beer_guid_equal(rfid, documents)) return BEER_KNOWN_FOLDER_DOCUMENTS;
    if (beer_guid_equal(rfid, saved_games)) return BEER_KNOWN_FOLDER_SAVED_GAMES;
    if (beer_guid_equal(rfid, roaming)) return BEER_KNOWN_FOLDER_ROAMING_APPDATA;
    if (beer_guid_equal(rfid, local)) return BEER_KNOWN_FOLDER_LOCAL_APPDATA;
    if (beer_guid_equal(rfid, program_data)) return BEER_KNOWN_FOLDER_PROGRAM_DATA;
    return BEER_KNOWN_FOLDER_UNKNOWN;
}

static inline int beer_creation_disposition_flags(uint32_t creation,
                                                   int *add_flags)
{
    enum {
        BEER_O_CREAT = 1 << 0,
        BEER_O_EXCL = 1 << 1,
        BEER_O_TRUNC = 1 << 2
    };
    if (!add_flags) return 0;
    switch (creation) {
    case 1: *add_flags = BEER_O_CREAT | BEER_O_EXCL; return 1; /* CREATE_NEW */
    case 2: *add_flags = BEER_O_CREAT | BEER_O_TRUNC; return 1; /* CREATE_ALWAYS */
    case 3: *add_flags = 0; return 1;                            /* OPEN_EXISTING */
    case 4: *add_flags = BEER_O_CREAT; return 1;                 /* OPEN_ALWAYS */
    case 5: *add_flags = BEER_O_TRUNC; return 1;                 /* TRUNCATE_EXISTING */
    default: *add_flags = 0; return 0;
    }
}

#endif
