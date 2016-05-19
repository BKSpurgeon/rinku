/*
 * Copyright (c) 2016, GitHub, Inc
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <ctype.h>

#include "buffer.h"
#include "autolink.h"

#if defined(_WIN32)
#define strncasecmp	_strnicmp
#endif

bool
autolink_issafe(const uint8_t *link, size_t link_len)
{
	static const size_t valid_uris_count = 5;
	static const char *valid_uris[] = {
		"/", "http://", "https://", "ftp://", "mailto:"
	};

	size_t i;

	for (i = 0; i < valid_uris_count; ++i) {
		size_t len = strlen(valid_uris[i]);

		if (link_len > len &&
			strncasecmp((char *)link, valid_uris[i], len) == 0 &&
			isalnum(link[len]))
			return true;
	}

	return false;
}

static bool
autolink_delim(const uint8_t *data, struct autolink_pos *link)
{
	uint8_t cclose, copen = 0;
	size_t i;

	for (i = link->start; i < link->end; ++i)
		if (data[i] == '<') {
			link->end = i;
			break;
		}

	while (link->end > link->start) {
		if (strchr("?!.,:", data[link->end - 1]) != NULL)
			link->end--;

		else if (data[link->end - 1] == ';') {
			size_t new_end = link->end - 2;

			while (new_end > 0 && isalpha(data[new_end]))
				new_end--;

			if (new_end < link->end - 2 && data[new_end] == '&')
				link->end = new_end;
			else
				link->end--;
		}
		else break;
	}

	if (link->end == link->start)
		return false;

	cclose = data[link->end - 1];

	switch (cclose) {
	case '"':	copen = '"'; break;
	case '\'':	copen = '\''; break;
	case ')':	copen = '('; break;
	case ']':	copen = '['; break;
	case '}':	copen = '{'; break;
	}

	if (copen != 0) {
		size_t closing = 0;
		size_t opening = 0;
		size_t i = link->start;

		/* Try to close the final punctuation sign in this same line;
		 * if we managed to close it outside of the URL, that means that it's
		 * not part of the URL. If it closes inside the URL, that means it
		 * is part of the URL.
		 *
		 * Examples:
		 *
		 *	foo http://www.pokemon.com/Pikachu_(Electric) bar
		 *		=> http://www.pokemon.com/Pikachu_(Electric)
		 *
		 *	foo (http://www.pokemon.com/Pikachu_(Electric)) bar
		 *		=> http://www.pokemon.com/Pikachu_(Electric)
		 *
		 *	foo http://www.pokemon.com/Pikachu_(Electric)) bar
		 *		=> http://www.pokemon.com/Pikachu_(Electric))
		 *
		 *	(foo http://www.pokemon.com/Pikachu_(Electric)) bar
		 *		=> foo http://www.pokemon.com/Pikachu_(Electric)
		 */

		while (i < link->end) {
			if (data[i] == copen)
				opening++;
			else if (data[i] == cclose)
				closing++;

			i++;
		}

		if (closing != opening)
			link->end--;
	}

	return true;
}

static bool
check_domain(const uint8_t *data, size_t size,
		struct autolink_pos *link, bool allow_short)
{
	size_t i, np = 0;

	if (!isalnum(data[link->start]))
		return false;

	for (i = link->start + 1; i < size - 1; ++i) {
		if (data[i] == '.') np++;
		else if (!isalnum(data[i]) && data[i] != '-') break;
	}

	link->end = i;

	if (allow_short) {
		/* We don't need a valid domain in the strict sense (with
		 * least one dot; so just make sure it's composed of valid
		 * domain characters and return the length of the the valid
		 * sequence. */
		return true;
	} else {
		/* a valid domain needs to have at least a dot.
		 * that's as far as we get */
		return (np > 0);
	}
}

bool
autolink__www(
	struct autolink_pos *link,
	const uint8_t *data,
	size_t pos,
	size_t size,
	unsigned int flags)
{
	if (pos > 0 && !ispunct(data[pos - 1]) && !isspace(data[pos - 1]))
		return false;

	if ((size - pos) < 4 ||
		memcmp(data + pos, "www.", strlen("www.")) != 0)
		return false;

	link->start = pos;
	link->end = 0;

	if (!check_domain(data, size, link, false))
		return false;

	while (link->end < size && !isspace(data[link->end]))
		link->end++;

	return autolink_delim(data, link);
}

bool
autolink__email(
	struct autolink_pos *link,
	const uint8_t *data,
	size_t pos,
	size_t size,
	unsigned int flags)
{
	int nb = 0, np = 0;

	link->start = pos;
	link->end = pos;

	for (; link->start > 0; link->start--) {
		uint8_t c = data[link->start - 1];

		if (isalnum(c))
			continue;

		if (strchr(".+-_", c) != NULL)
			continue;

		break;
	}

	if (link->start == pos)
		return false;

	for (; link->end < size; link->end++) {
		uint8_t c = data[link->end];

		if (isalnum(c))
			continue;

		if (c == '@')
			nb++;
		else if (c == '.' && link->end < size - 1)
			np++;
		else if (c != '-' && c != '_')
			break;
	}

	if ((link->end - pos) < 2 || nb != 1 || np == 0)
		return false;

	return autolink_delim(data, link);
}

bool
autolink__url(
	struct autolink_pos *link,
	const uint8_t *data,
	size_t pos,
	size_t size,
	unsigned int flags)
{
	assert(data[pos] == ':');

	if ((size - pos) < 4 || data[pos + 1] != '/' || data[pos + 2] != '/')
		return false;

	link->start = pos + 3;
	link->end = 0;

	if (!check_domain(data, size, link, flags & AUTOLINK_SHORT_DOMAINS))
		return false;

	while (link->end < size && !isspace(data[link->end]))
		link->end++;

	link->start = pos;
	while (link->start && isalpha(data[link->start - 1]))
		link->start--;

	if (!autolink_issafe(data + link->start, size - link->start))
		return false;

	return autolink_delim(data, link);
}
