#ifndef TINYALSA_VERSION_H
#define TINYALSA_VERSION_H

/* Macros for expanding the version numbers into string literals */
#define TINYALSA_VERSION_STR_EX(number) #number
#define TINYALSA_VERSION_STR(number) TINYALSA_VERSION_STR_EX (number)

#define TINYALSA_VERSION_MAJOR 2

#define TINYALSA_VERSION_MINOR 0

#define TINYALSA_VERSION_PATCH 0

/* The final version number is constructed based on minor, major and patch */
#define TINYALSA_VERSION \
    ((unsigned long) \
    ((TINYALSA_VERSION_MAJOR << 16)   | \
     (TINYALSA_VERSION_MINOR << 8 )   | \
     (TINYALSA_VERSION_PATCH      )))

/* The version string is constructed by concatenating individual ver. strings */
#define TINYALSA_VERSION_STRING \
    TINYALSA_VERSION_STR (TINYALSA_VERSION_MAJOR) \
    "." \
    TINYALSA_VERSION_STR (TINYALSA_VERSION_MINOR) \
    "." \
    TINYALSA_VERSION_STR (TINYALSA_VERSION_PATCH)

#endif /* TINYALSA_VERSION_H */