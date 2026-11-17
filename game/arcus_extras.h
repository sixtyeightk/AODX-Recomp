/*
 * arcus_extras.h — the extras embedded by the build ("books": the original manual, the game
 * guide, magazine reviews), from extras-cache/books/ (see CMakeLists.txt). Never committed.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { ARCUS_BOOK_PAGES, ARCUS_BOOK_SCROLL };     /* fitted pages / one tall scrolling page */

typedef struct {
    int kind;              /* ARCUS_BOOK_* */
    char group[16];        /* "manual", "manual-jp", "guide", "art", "review" */
    char title[40];        /* e.g. "GamePro (US)" */
    char detail[64];       /* a review: "September 1991|96" (issue, score); art: a caption */
    int first, count;      /* its pages in the embedded blob */
} ArcusBook;

/* Books in this build (0 with ARCUS_NO_EXTRAS=1, as a build without any). */
int arcus_extras_books(void);
const ArcusBook *arcus_extras_book(int i);
/* The first book of a group, or -1. */
int arcus_extras_find(const char *group);
/* Count of books in a group. */
int arcus_extras_group_count(const char *group);
/* Page `page` (0-based) of a book: its JPEG bytes. 0 if out of range. */
int arcus_extras_page(int book, int page, const uint8_t **data, size_t *len);
