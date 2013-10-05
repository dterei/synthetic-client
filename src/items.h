// the memcached item (key-value) representation and storage.
#ifndef _ITEMS_H
#define _ITEMS_H

#include <stdint.h>
#include <unistd.h>

#define ITEM_key(item)    ((char*) &((item)->data))
#define ITEM_suffix(item) ((char*) &((item)->data) + (item)->nkey + 1)

// Structure for storing items within memcached.
//
// Memory Storage:
//   <struct _stritem>
//   "<key> \0" (trailing space + null-terminated)
//   "<flags and data-length>\r\n" (no null-terminator)
//   "<data>\r\n" (no terminating null; it's binary!)
//
// Stored this way as a get response is formatted:
//   "VALUE <key> <flags> <data_length>\r\n"
//   "<data>\r\n"
//
// For example:
//   "VALUE key_12 0 5\r\n"
//   "hello\r\n"
// 
// The key is stored a little unusually. We store it with a single
// trailing-space and a null-terminator. However, we don't include the
// null-terminator in the stored length (nkey) as we skip over it normally. We
// only have it for ease of matching when doing key lookups.
//
// Also, we actually store the flags and data-length as one string, preprepared
// for transmission.
//
//                                           item.data+nkey+1+nsuffix+nbytes
//                                                                         v
// [<item>|<key>.....|<flags_and_data-length_str>.....|<data>..............]
// ^      ^          ^                                ^
// item   item.data  item.data+nkey+1                 item.data+nkey+1+nsuffix
//
typedef struct _stritem {
	uint8_t nkey;    // key length including padding but not null-terminator.
	uint8_t nsuffix; // length of flags_and_data-length string.
	int     nbytes;  // size of data.
	char    data[];  // pointer to trailing data.
} item;

item *item_get(const char *key, const size_t nkey);
void item_remove(item *item);
void item_update(item *item);

#endif

