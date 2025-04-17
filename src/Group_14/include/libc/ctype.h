/**
 * @file libc/ctype.h
 * @brief Basic character classification and conversion functions.
 *
 * Provides minimal implementations for standard <ctype.h> functions
 * suitable for a freestanding kernel environment.
 */

 #ifndef _LIBC_CTYPE_H
 #define _LIBC_CTYPE_H
 
 #ifdef __cplusplus
 extern "C" {
 #endif
 
 /**
  * @brief Checks if the given character is a decimal digit ('0'-'9').
  * @param c The character to check (passed as int).
  * @return Non-zero if the character is a digit, 0 otherwise.
  */
 static inline int isdigit(int c) {
     return (c >= '0' && c <= '9');
 }
 
 /**
  * @brief Checks if the given character is an uppercase letter ('A'-'Z').
  * @param c The character to check (passed as int).
  * @return Non-zero if the character is an uppercase letter, 0 otherwise.
  */
 static inline int isupper(int c) {
     return (c >= 'A' && c <= 'Z');
 }
 
 /**
  * @brief Checks if the given character is a lowercase letter ('a'-'z').
  * @param c The character to check (passed as int).
  * @return Non-zero if the character is a lowercase letter, 0 otherwise.
  */
 static inline int islower(int c) {
     return (c >= 'a' && c <= 'z');
 }
 
 /**
  * @brief Checks if the given character is an alphabetic letter (A-Z or a-z).
  * @param c The character to check (passed as int).
  * @return Non-zero if the character is a letter, 0 otherwise.
  */
 static inline int isalpha(int c) {
     return isupper(c) || islower(c);
 }
 
 /**
  * @brief Checks if the given character is alphanumeric (letter or digit).
  * @param c The character to check (passed as int).
  * @return Non-zero if the character is alphanumeric, 0 otherwise.
  */
 static inline int isalnum(int c) {
     return isalpha(c) || isdigit(c);
 }
 
 /**
  * @brief Checks if the given character is a whitespace character.
  * Includes space, form feed, newline, carriage return, horizontal tab, vertical tab.
  * @param c The character to check (passed as int).
  * @return Non-zero if the character is whitespace, 0 otherwise.
  */
 static inline int isspace(int c) {
     return (c == ' ' || c == '\f' || c == '\n' || c == '\r' || c == '\t' || c == '\v');
 }
 
 /**
  * @brief Checks if the given character is printable (including space).
  * Typically ASCII characters 0x20 through 0x7E.
  * @param c The character to check (passed as int).
  * @return Non-zero if the character is printable, 0 otherwise.
  */
 static inline int isprint(int c) {
     return (c >= 0x20 && c <= 0x7E);
 }
 
 /**
  * @brief Converts a lowercase letter to its corresponding uppercase letter.
  * If the character is not a lowercase letter, it's returned unchanged.
  * @param c The character to convert (passed as int).
  * @return The uppercase equivalent or the original character.
  */
 static inline int toupper(int c) {
     if (islower(c)) {
         return c - 'a' + 'A';
     }
     return c;
 }
 
 /**
  * @brief Converts an uppercase letter to its corresponding lowercase letter.
  * If the character is not an uppercase letter, it's returned unchanged.
  * @param c The character to convert (passed as int).
  * @return The lowercase equivalent or the original character.
  */
 static inline int tolower(int c) {
     if (isupper(c)) {
         return c - 'A' + 'a';
     }
     return c;
 }
 
 
 #ifdef __cplusplus
 }
 #endif
 
 #endif // _LIBC_CTYPE_H
 