// the memcached item (key-value) representation and storage.
#include "items.h"

#include <unistd.h>

item fixed_item =
	{ .nkey    = 4,
	  .nsuffix = 5,
	  .nbytes  = 7,
	  .data    = "key \0000 5\r\nhello\r\n"
	};

// lookup a key-value.
item *item_get(const char *key, const size_t nkey) {
	return &fixed_item;
}

// decrease the ref count on the item and add to free-list if 0.
void item_remove(item *item) {
	// noop
}

// update an items position in the LRU.
void item_update(item *item) {
	// noop
}

