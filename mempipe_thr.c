/* Simple shared-memory IPC throughput test, with spin-waits rather
   than blocking.  The shared area is divided into packets, each of
   which starts with a header.  The header is the size of a cache
   line, but the only really interesting field is the size of the
   packet.  To transmit, you just copy the data you want to transmit
   it and then go back and write the header.  Meanwhile, the receiver
   will be sitting spinning waiting for the header to arrive, and will
   read the packet out as soon as it does.  Once it's done so, it'll
   go back and set the finished flag in the header, so that the
   transmitter knows when it's safe to reuse a bit of buffer for a new
   message.  The benchmark which we build on top of this just keeps
   sending fixed-sized messages for soem number of times, waiting as
   appropriate for the receiver to pick them up. */
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <assert.h>
#include <err.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "test.h"
#include "xutil.h"

#define PAGE_SIZE 4096
#define CACHE_LINE_SIZE 64
static unsigned nr_shared_pages = 512;
#define ring_size (PAGE_SIZE * nr_shared_pages)

struct msg_header {
  int size;
  int pad[CACHE_LINE_SIZE / sizeof(int) - 1];
};

static unsigned long
mask_ring_index(unsigned long idx)
{
  return idx & (ring_size - 1);
}

static void
init_test(test_data *td)
{
  td->data = establish_shm_segment(nr_shared_pages);
}

static void
consume_message(const void *data, unsigned long offset, unsigned size,
		void *outbuf)
{
  offset = mask_ring_index(offset);
  if (offset + size <= ring_size) {
    memcpy(outbuf, data + offset, size);
  } else {
    memcpy(outbuf, data + offset, ring_size - offset);
    memcpy(outbuf + ring_size - offset, data, size - (ring_size - offset));
  }
}

static void
populate_message(void *data, unsigned long offset, unsigned size, const void *inbuf)
{
  offset = mask_ring_index(offset);
  if (offset + size <= ring_size) {
    memcpy(data + offset, inbuf, size);
  } else {
    memcpy(data + offset, inbuf, ring_size - offset);
    memcpy(data, inbuf + (ring_size - offset), size - (ring_size - offset));
  }
}

static void
run_child(test_data *td)
{
  unsigned long next_message_start;
  volatile struct msg_header *mh = td->data;
  int sz;
  char *buf = xmalloc(td->size);

  /* Sync up with parent */
  mh->size = 0xf001;
  while (mh->size == 0xf001)
    ;

  next_message_start = 0;
  /* Enter main message loop */
  while (1) {
    mh = td->data + mask_ring_index(next_message_start);
    while (mh->size <= 0)
      ;
    sz = mh->size;
    if (sz == 1) /* End of test; normal messages are multiples of
		    cache line size. */
      break;
    assert(sz == td->size);
    consume_message(td->data, next_message_start + sizeof(struct msg_header), sz, buf);
    mh->size = -sz;
    next_message_start += sz + sizeof(struct msg_header);
  }
}

static void
run_parent(test_data *td)
{
  volatile struct msg_header *mh = td->data;
  unsigned long next_tx_offset;
  unsigned long first_unacked_msg;
  char *buf = xmalloc(td->size);

  assert(td->size < ring_size - sizeof(struct msg_header));

  /* Wait for child to show up. */
  while (mh->size != 0xf001)
    ;
  mh->size = 0;

  next_tx_offset = 0;
  first_unacked_msg = 0;

  /* Round up to multiple of cache line size, for sanity. */
  td->size = td->size + CACHE_LINE_SIZE - 1;
  td->size -= td->size % CACHE_LINE_SIZE;

  thr_test(
    do {
      /* Check for available ring space (eom = end of message) */
      unsigned long eom = next_tx_offset + td->size + sizeof(struct msg_header);
      while (eom - first_unacked_msg > ring_size) {
	int size;
	mh = td->data + mask_ring_index(first_unacked_msg);
	do {
	  size = mh->size;
	} while (size > 0);
	assert(size < 0);
	assert(size % CACHE_LINE_SIZE == 0);
	first_unacked_msg += -size + sizeof(struct msg_header);
      }
      /* Send message */
      mh = td->data + mask_ring_index(next_tx_offset);
      populate_message(td->data, next_tx_offset + sizeof(struct msg_header), td->size, buf);
      mh->size = td->size;
      next_tx_offset += td->size + sizeof(struct msg_header);
    } while(0),
    do {
      /* Wait for child to acknowledge receipt of all messages */
      while (first_unacked_msg != next_tx_offset) {
	int size;
	mh = td->data + mask_ring_index(first_unacked_msg);
	do {
	  size = mh->size;
	} while (size > 0);
	first_unacked_msg += -size + sizeof(struct msg_header);
      }
    } while (0),
    td);

  /* Tell child to go away */
  mh = td->data + mask_ring_index(next_tx_offset);
  mh->size = 1;
}

int
main(int argc, char *argv[])
{
  test_t t = { "mempipe_thr", init_test, run_parent, run_child };
  char *ring_order = getenv("MEMPIPE_RING_ORDER");
  if (ring_order) {
    int as_int;
    if (sscanf(ring_order, "%d", &as_int) != 1)
      err(1, "MEMPIPE_RING_ORDER must be an integer");
    if (as_int < 0 || as_int > 15)
      errx(1, "MEMPIPE_RING_ORDER must be between 0 and 15");
    nr_shared_pages = 1 << as_int;
  }
  run_test(argc, argv, &t);
  return 0;
}
