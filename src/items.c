// the memcached item (key-value) representation and storage.
#include "items.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static item *fixed_item = NULL;
static const char *fixed_value = "key \0000 5\r\nhello\r\n";

void item_init_system(void) {
	fixed_item = (item *) malloc(offsetof(struct _stritem, data[7]));
	fixed_item->nkey = 4;
	fixed_item->nsuffix = 5;
	fixed_item->nbytes = 7;
	memcpy(fixed_item->data, fixed_value, 7);
}

// lookup a key-value.
item *item_get(const char *key, const size_t nkey) {
	return fixed_item;
}

// decrease the ref count on the item and add to free-list if 0.
void item_remove(item *item) {
	// noop
}

// update an items position in the LRU.
void item_update(item *item) {
	// noop
}

