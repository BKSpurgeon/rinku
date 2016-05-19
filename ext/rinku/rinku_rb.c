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
#include <stdio.h>

#define RUBY_EXPORT __attribute__ ((visibility ("default")))

#include <ruby.h>
#include <ruby/encoding.h>

#include "rinku.h"
#include "autolink.h"

static VALUE rb_mRinku;

static void
autolink_callback(struct buf *link_text,
		const uint8_t *url, size_t url_len, void *block)
{
	VALUE rb_link, rb_link_text;
	rb_link = rb_str_new((const char *)url, url_len);
	rb_link_text = rb_funcall((VALUE)block, rb_intern("call"), 1, rb_link);
	Check_Type(rb_link_text, T_STRING);
	bufput(link_text, RSTRING_PTR(rb_link_text), RSTRING_LEN(rb_link_text));
}

const char **rinku_load_tags(VALUE rb_skip)
{
	const char **skip_tags;
	size_t i, count;

	Check_Type(rb_skip, T_ARRAY);

	count = RARRAY_LEN(rb_skip);
	skip_tags = xmalloc(sizeof(void *) * (count + 1));

	for (i = 0; i < count; ++i) {
		VALUE tag = rb_ary_entry(rb_skip, i);
		Check_Type(tag, T_STRING);
		skip_tags[i] = StringValueCStr(tag);
	}

	skip_tags[count] = NULL;
	return skip_tags;
}

/*
 * Document-method: auto_link
 *
 * call-seq:
 *  auto_link(text, mode=:all, link_attr=nil, skip_tags=nil, flags=0)
 *  auto_link(text, mode=:all, link_attr=nil, skip_tags=nil, flags=0) { |link_text| ... }
 *
 * Parses a block of text looking for "safe" urls or email addresses,
 * and turns them into HTML links with the given attributes.
 *
 * NOTE: The block of text may or may not be HTML; if the text is HTML,
 * Rinku will skip the relevant tags to prevent double-linking and linking
 * inside `pre` blocks by default.
 *
 * NOTE: If the input text is HTML, it's expected to be already escaped.
 * Rinku will perform no escaping.
 *
 * NOTE: Currently the follow protocols are considered safe and are the
 * only ones that will be autolinked.
 *
 *     http:// https:// ftp:// mailto://
 *
 * Email addresses are also autolinked by default. URLs without a protocol
 * specifier but starting with 'www.' will also be autolinked, defaulting to
 * the 'http://' protocol.
 *
 * -   `text` is a string in plain text or HTML markup. If the string is formatted in
 * HTML, Rinku is smart enough to skip the links that are already enclosed in `<a>`
 * tags.`
 *
 * -   `mode` is a symbol, either `:all`, `:urls` or `:email_addresses`, 
 * which specifies which kind of links will be auto-linked. 
 *
 * -   `link_attr` is a string containing the link attributes for each link that
 * will be generated. These attributes are not sanitized and will be include as-is
 * in each generated link, e.g.
 *
 *      ~~~~~ruby
 *      auto_link('http://www.pokemon.com', :all, 'target="_blank"')
 *      # => '<a href="http://www.pokemon.com" target="_blank">http://www.pokemon.com</a>'
 *      ~~~~~
 *
 *     This string can be autogenerated from a hash using the Rails `tag_options` helper.
 *
 * -   `skip_tags` is a list of strings with the names of HTML tags that will be skipped
 * when autolinking. If `nil`, this defaults to the value of the global `Rinku.skip_tags`,
 * which is initially `["a", "pre", "code", "kbd", "script"]`.
 *
 * -   `flag` is an optional boolean value specifying whether to recognize
 * 'http://foo' as a valid domain, or require at least one '.'. It defaults to false.
 *
 * -   `&block` is an optional block argument. If a block is passed, it will
 * be yielded for each found link in the text, and its return value will be used instead
 * of the name of the link. E.g.
 *
 *     ~~~~~ruby
 *     auto_link('Check it out at http://www.pokemon.com') do |url|
 *       "THE POKEMAN WEBSITEZ"
 *     end
 *     # => 'Check it out at <a href="http://www.pokemon.com">THE POKEMAN WEBSITEZ</a>'
 *     ~~~~~~
 */
static VALUE
rb_rinku_autolink(int argc, VALUE *argv, VALUE self)
{
	static const char *SKIP_TAGS[] = {"a", "pre", "code", "kbd", "script", NULL};

	VALUE result, rb_text, rb_mode, rb_html, rb_skip, rb_flags, rb_block;
	struct buf *output_buf;
	int link_mode, count;
	unsigned int link_flags = 0;
	const char *link_attr = NULL;
	const char **skip_tags = NULL;
	ID mode_sym;

	rb_scan_args(argc, argv, "14&", &rb_text, &rb_mode,
		&rb_html, &rb_skip, &rb_flags, &rb_block); 

	Check_Type(rb_text, T_STRING);

	if (!NIL_P(rb_mode)) {
		Check_Type(rb_mode, T_SYMBOL);
		mode_sym = SYM2ID(rb_mode);
	} else {
		mode_sym = rb_intern("all");
	}

	if (!NIL_P(rb_html)) {
		Check_Type(rb_html, T_STRING);
		link_attr = RSTRING_PTR(rb_html);
	}

	if (NIL_P(rb_skip))
		rb_skip = rb_iv_get(self, "@skip_tags");

	if (NIL_P(rb_skip)) {
		skip_tags = SKIP_TAGS;
	} else {
		skip_tags = rinku_load_tags(rb_skip);
	}

	if (!NIL_P(rb_flags)) {
		Check_Type(rb_flags, T_FIXNUM);
		link_flags = FIX2INT(rb_flags);
	}

	output_buf = bufnew(32);

	if (mode_sym == rb_intern("all"))
		link_mode = AUTOLINK_ALL;
	else if (mode_sym == rb_intern("email_addresses"))
		link_mode = AUTOLINK_EMAILS;
	else if (mode_sym == rb_intern("urls"))
		link_mode = AUTOLINK_URLS;
	else
		rb_raise(rb_eTypeError,
			"Invalid linking mode (possible values are :all, :urls, :email_addresses)");

	count = rinku_autolink(
		output_buf,
		(const uint8_t *)RSTRING_PTR(rb_text),
		(size_t)RSTRING_LEN(rb_text),
		link_mode,
		link_flags,
		link_attr,
		skip_tags,
		RTEST(rb_block) ? &autolink_callback : NULL,
		(void*)rb_block);

	if (count == 0)
		result = rb_text;
	else {
		result = rb_str_new((char *)output_buf->data, output_buf->size);
		rb_enc_copy(result, rb_text);
	}

	if (skip_tags != SKIP_TAGS)
		xfree(skip_tags);

	bufrelease(output_buf);
	return result;
}

void RUBY_EXPORT Init_rinku()
{
	rb_mRinku = rb_define_module("Rinku");
	rb_define_module_function(rb_mRinku, "auto_link", rb_rinku_autolink, -1);
	rb_define_const(rb_mRinku, "AUTOLINK_SHORT_DOMAINS", INT2FIX(AUTOLINK_SHORT_DOMAINS));
}

