/* Copyright 2013-2014 IBM Corp.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * 	http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/* This example code shows how to read from the trace buffer. */
#include <external/trace/trace.h>
#include <trace_types.h>
#include <errno.h>

bool trace_empty(const struct tracebuf *tb)
{
	const struct trace_repeat *rep;

	if (tb->rpos == tb->end)
		return true;

	/*
	 * If we have a single element only, and it's a repeat buffer
	 * we've already seen every repeat for (yet which may be
	 * incremented in future), we're also empty.
	 */
	rep = (void *)tb->buf + (tb->rpos & tb->mask);
	if (tb->end != tb->rpos + sizeof(*rep))
		return false;

	if (rep->type != TRACE_REPEAT)
		return false;

	if (rep->num != tb->last_repeat)
		return false;

	return true;
}

/* You can't read in parallel, so some locking required in caller. */
bool trace_get(union trace *t, struct tracebuf *tb)
{
	u64 start;
	size_t len = sizeof(*t) < tb->max_size ? sizeof(*t) : tb->max_size;

	if (trace_empty(tb))
		return false;

again:
	/*
	 * The actual buffer is slightly larger than tbsize, so this
	 * memcpy is always valid.
	 */
	memcpy(t, tb->buf + (tb->rpos & tb->mask), len);

	rmb(); /* read barrier, so we read tb->start after copying record. */

	start = tb->start;

	/* Now, was that overwritten? */
	if (tb->rpos < start) {
		/* Create overflow record. */
		t->overflow.unused64 = 0;
		t->overflow.type = TRACE_OVERFLOW;
		t->overflow.len_div_8 = sizeof(t->overflow) / 8;
		t->overflow.bytes_missed = start - tb->rpos;
		tb->rpos += t->overflow.bytes_missed;
		return true;
	}

	/* Repeat entries need special handling */
	if (t->hdr.type == TRACE_REPEAT) {
		u32 num = t->repeat.num;

		/* In case we've read some already... */
		t->repeat.num -= tb->last_repeat;

		/* Record how many repeats we saw this time. */
		tb->last_repeat = num;

		/* Don't report an empty repeat buffer. */
		if (t->repeat.num == 0) {
			/*
			 * This can't be the last buffer, otherwise
			 * trace_empty would have returned true.
			 */
			assert(tb->end > tb->rpos + t->hdr.len_div_8 * 8);
			/* Skip to next entry. */
			tb->rpos += t->hdr.len_div_8 * 8;
			tb->last_repeat = 0;
			goto again;
		}
	} else {
		tb->last_repeat = 0;
		tb->rpos += t->hdr.len_div_8 * 8;
	}

	return true;
}
