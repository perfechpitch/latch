http_parser Modifications
--------------------
1. move 'enum state' from http_parser.c to http_parser.h
3. http_parser.c ignore warning: "-Wconversion", "-Wsign-conversion"

llhttp Modifications
--------------------
1. llhttp_api.c ignore warning: "-Wconversion", "-Wsign-conversion"
2. llhttp_url.c ignore warning: "-Wconversion", "-Wsign-conversion"
3. llhttp_internal.c ignore warning: "-Wconversion", "-Wsign-conversion" "-Wunused-variable" "-Wunreachable-code"
4. llhttp_url.h : LLHTTP_STRICT_MODE set default value 1
5. llhttp.h : delete codes below
    #elif defined(_WIN32)
    #define LLHTTP_EXPORT __declspec(dllexport)