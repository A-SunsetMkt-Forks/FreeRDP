/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 *
 * Copyright 2014 Thincast Technologies GmbH
 * Copyright 2014 Hardening <contact@hardening-consulting.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <string.h>

#include <freerdp/utils/ringbuffer.h>

static BOOL test_overlaps(void)
{
	RingBuffer rb;
	DataChunk chunks[2];
	BYTE bytes[200];
	int nchunks = 0;
	int counter = 0;

	for (size_t i = 0; i < sizeof(bytes); i++)
		bytes[i] = (BYTE)i;

	ringbuffer_init(&rb, 5);
	if (!ringbuffer_write(&rb, bytes, 4)) /* [0123.] */
		goto error;
	counter += 4;
	ringbuffer_commit_read_bytes(&rb, 2); /* [..23.] */

	if (!ringbuffer_write(&rb, &bytes[counter], 2)) /* [5.234] */
		goto error;
	counter += 2;

	nchunks = ringbuffer_peek(&rb, chunks, 4);
	if (nchunks != 2 || chunks[0].size != 3 || chunks[1].size != 1)
		goto error;

	for (int x = 0, j = 2; x < nchunks; x++)
	{
		for (size_t k = 0; k < chunks[x].size; k++, j++)
		{
			if (chunks[x].data[k] != (BYTE)j)
				goto error;
		}
	}

	ringbuffer_commit_read_bytes(&rb, 3); /* [5....] */
	if (ringbuffer_used(&rb) != 1)
		goto error;

	if (!ringbuffer_write(&rb, &bytes[counter], 6)) /* [56789ab....] */
		goto error;

	ringbuffer_commit_read_bytes(&rb, 6); /* [......b....] */
	nchunks = ringbuffer_peek(&rb, chunks, 10);
	if (nchunks != 1 || chunks[0].size != 1 || (*chunks[0].data != 0xb))
		goto error;

	if (ringbuffer_capacity(&rb) != 5)
		goto error;

	ringbuffer_destroy(&rb);
	return TRUE;
error:
	ringbuffer_destroy(&rb);
	return FALSE;
}

int TestRingBuffer(int argc, char* argv[])
{
	RingBuffer ringBuffer;
	int testNo = 0;
	BYTE* tmpBuf = NULL;
	BYTE* rb_ptr = NULL;
	DataChunk chunks[2];

	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);

	if (!ringbuffer_init(&ringBuffer, 10))
	{
		(void)fprintf(stderr, "unable to initialize ringbuffer\n");
		return -1;
	}

	tmpBuf = (BYTE*)malloc(50);
	if (!tmpBuf)
		return -1;

	for (int i = 0; i < 50; i++)
		tmpBuf[i] = (char)i;

	(void)fprintf(stderr, "%d: basic tests...", ++testNo);
	if (!ringbuffer_write(&ringBuffer, tmpBuf, 5) || !ringbuffer_write(&ringBuffer, tmpBuf, 5) ||
	    !ringbuffer_write(&ringBuffer, tmpBuf, 5))
	{
		(void)fprintf(stderr, "error when writing bytes\n");
		return -1;
	}

	if (ringbuffer_used(&ringBuffer) != 15)
	{
		(void)fprintf(stderr, "invalid used size got %" PRIuz " when I would expect 15\n",
		              ringbuffer_used(&ringBuffer));
		return -1;
	}

	if (ringbuffer_peek(&ringBuffer, chunks, 10) != 1 || chunks[0].size != 10)
	{
		(void)fprintf(stderr, "error when reading bytes\n");
		return -1;
	}
	ringbuffer_commit_read_bytes(&ringBuffer, chunks[0].size);

	/* check retrieved bytes */
	for (size_t i = 0; i < chunks[0].size; i++)
	{
		if (chunks[0].data[i] != i % 5)
		{
			(void)fprintf(stderr,
			              "invalid byte at %" PRIuz ", got %" PRIu8 " instead of %" PRIuz "\n", i,
			              chunks[0].data[i], i % 5U);
			return -1;
		}
	}

	if (ringbuffer_used(&ringBuffer) != 5)
	{
		(void)fprintf(stderr, "invalid used size after read got %" PRIuz " when I would expect 5\n",
		              ringbuffer_used(&ringBuffer));
		return -1;
	}

	/* write some more bytes to have writePtr < readPtr and data split in 2 chunks */
	if (!ringbuffer_write(&ringBuffer, tmpBuf, 6) ||
	    ringbuffer_peek(&ringBuffer, chunks, 11) != 2 || chunks[0].size != 10 ||
	    chunks[1].size != 1)
	{
		(void)fprintf(stderr, "invalid read of split data\n");
		return -1;
	}

	ringbuffer_commit_read_bytes(&ringBuffer, 11);
	(void)fprintf(stderr, "ok\n");

	(void)fprintf(stderr, "%d: peek with nothing to read...", ++testNo);
	if (ringbuffer_peek(&ringBuffer, chunks, 10))
	{
		(void)fprintf(stderr, "peek returns some chunks\n");
		return -1;
	}
	(void)fprintf(stderr, "ok\n");

	(void)fprintf(stderr, "%d: ensure_linear_write / read() shouldn't grow...", ++testNo);
	for (int i = 0; i < 1000; i++)
	{
		rb_ptr = ringbuffer_ensure_linear_write(&ringBuffer, 50);
		if (!rb_ptr)
		{
			(void)fprintf(stderr, "ringbuffer_ensure_linear_write() error\n");
			return -1;
		}

		memcpy(rb_ptr, tmpBuf, 50);

		if (!ringbuffer_commit_written_bytes(&ringBuffer, 50))
		{
			(void)fprintf(stderr, "ringbuffer_commit_written_bytes() error, i=%d\n", i);
			return -1;
		}

		// ringbuffer_commit_read_bytes(&ringBuffer, 25);
	}

	for (int i = 0; i < 1000; i++)
		ringbuffer_commit_read_bytes(&ringBuffer, 25);

	for (int i = 0; i < 1000; i++)
		ringbuffer_commit_read_bytes(&ringBuffer, 25);

	if (ringbuffer_capacity(&ringBuffer) != 10)
	{
		(void)fprintf(stderr, "not the expected capacity, have %" PRIuz " and expects 10\n",
		              ringbuffer_capacity(&ringBuffer));
		return -1;
	}
	(void)fprintf(stderr, "ok\n");

	(void)fprintf(stderr, "%d: free size is correctly computed...", ++testNo);
	for (int i = 0; i < 1000; i++)
	{
		ringbuffer_ensure_linear_write(&ringBuffer, 50);
		if (!ringbuffer_commit_written_bytes(&ringBuffer, 50))
		{
			(void)fprintf(stderr, "ringbuffer_commit_written_bytes() error, i=%d\n", i);
			return -1;
		}
	}
	ringbuffer_commit_read_bytes(&ringBuffer, 50ULL * 1000ULL);
	(void)fprintf(stderr, "ok\n");

	ringbuffer_destroy(&ringBuffer);

	(void)fprintf(stderr, "%d: specific overlaps test...", ++testNo);
	if (!test_overlaps())
	{
		(void)fprintf(stderr, "ko\n");
		return -1;
	}
	(void)fprintf(stderr, "ok\n");

	ringbuffer_destroy(&ringBuffer);
	free(tmpBuf);
	return 0;
}
