#define GREETING "Hello"
char *s1 = GREETING ", " "World";
#define STR(x) #x
#define XSTR(x) STR(x)
#define VERSION_MAJOR 1
#define VERSION_MINOR 2
char *ver = XSTR(VERSION_MAJOR) "." XSTR(VERSION_MINOR);
