#include "cache.h"
#include "tag.h"
#include "commit.h"
#include "pkt-line.h"
#include "utf8.h"
#include "interpolate.h"
#include "diff.h"
#include "revision.h"

int save_commit_buffer = 1;

const char *commit_type = "commit";

static struct cmt_fmt_map {
	const char *n;
	size_t cmp_len;
	enum cmit_fmt v;
} cmt_fmts[] = {
	{ "raw",	1,	CMIT_FMT_RAW },
	{ "medium",	1,	CMIT_FMT_MEDIUM },
	{ "short",	1,	CMIT_FMT_SHORT },
	{ "email",	1,	CMIT_FMT_EMAIL },
	{ "full",	5,	CMIT_FMT_FULL },
	{ "fuller",	5,	CMIT_FMT_FULLER },
	{ "oneline",	1,	CMIT_FMT_ONELINE },
	{ "format:",	7,	CMIT_FMT_USERFORMAT},
};

static char *user_format;

enum cmit_fmt get_commit_format(const char *arg)
{
	int i;

	if (!arg || !*arg)
		return CMIT_FMT_DEFAULT;
	if (*arg == '=')
		arg++;
	if (!prefixcmp(arg, "format:")) {
		if (user_format)
			free(user_format);
		user_format = xstrdup(arg + 7);
		return CMIT_FMT_USERFORMAT;
	}
	for (i = 0; i < ARRAY_SIZE(cmt_fmts); i++) {
		if (!strncmp(arg, cmt_fmts[i].n, cmt_fmts[i].cmp_len) &&
		    !strncmp(arg, cmt_fmts[i].n, strlen(arg)))
			return cmt_fmts[i].v;
	}

	die("invalid --pretty format: %s", arg);
}

static struct commit *check_commit(struct object *obj,
				   const unsigned char *sha1,
				   int quiet)
{
	if (obj->type != OBJ_COMMIT) {
		if (!quiet)
			error("Object %s is a %s, not a commit",
			      sha1_to_hex(sha1), typename(obj->type));
		return NULL;
	}
	return (struct commit *) obj;
}

struct commit *lookup_commit_reference_gently(const unsigned char *sha1,
					      int quiet)
{
	struct object *obj = deref_tag(parse_object(sha1), NULL, 0);

	if (!obj)
		return NULL;
	return check_commit(obj, sha1, quiet);
}

struct commit *lookup_commit_reference(const unsigned char *sha1)
{
	return lookup_commit_reference_gently(sha1, 0);
}

struct commit *lookup_commit(const unsigned char *sha1)
{
	struct object *obj = lookup_object(sha1);
	if (!obj)
		return create_object(sha1, OBJ_COMMIT, alloc_commit_node());
	if (!obj->type)
		obj->type = OBJ_COMMIT;
	return check_commit(obj, sha1, 0);
}

static unsigned long parse_commit_date(const char *buf)
{
	unsigned long date;

	if (memcmp(buf, "author", 6))
		return 0;
	while (*buf++ != '\n')
		/* nada */;
	if (memcmp(buf, "committer", 9))
		return 0;
	while (*buf++ != '>')
		/* nada */;
	date = strtoul(buf, NULL, 10);
	if (date == ULONG_MAX)
		date = 0;
	return date;
}

static struct commit_graft **commit_graft;
static int commit_graft_alloc, commit_graft_nr;

static int commit_graft_pos(const unsigned char *sha1)
{
	int lo, hi;
	lo = 0;
	hi = commit_graft_nr;
	while (lo < hi) {
		int mi = (lo + hi) / 2;
		struct commit_graft *graft = commit_graft[mi];
		int cmp = hashcmp(sha1, graft->sha1);
		if (!cmp)
			return mi;
		if (cmp < 0)
			hi = mi;
		else
			lo = mi + 1;
	}
	return -lo - 1;
}

int register_commit_graft(struct commit_graft *graft, int ignore_dups)
{
	int pos = commit_graft_pos(graft->sha1);

	if (0 <= pos) {
		if (ignore_dups)
			free(graft);
		else {
			free(commit_graft[pos]);
			commit_graft[pos] = graft;
		}
		return 1;
	}
	pos = -pos - 1;
	if (commit_graft_alloc <= ++commit_graft_nr) {
		commit_graft_alloc = alloc_nr(commit_graft_alloc);
		commit_graft = xrealloc(commit_graft,
					sizeof(*commit_graft) *
					commit_graft_alloc);
	}
	if (pos < commit_graft_nr)
		memmove(commit_graft + pos + 1,
			commit_graft + pos,
			(commit_graft_nr - pos - 1) *
			sizeof(*commit_graft));
	commit_graft[pos] = graft;
	return 0;
}

struct commit_graft *read_graft_line(char *buf, int len)
{
	/* The format is just "Commit Parent1 Parent2 ...\n" */
	int i;
	struct commit_graft *graft = NULL;

	if (buf[len-1] == '\n')
		buf[--len] = 0;
	if (buf[0] == '#' || buf[0] == '\0')
		return NULL;
	if ((len + 1) % 41) {
	bad_graft_data:
		error("bad graft data: %s", buf);
		free(graft);
		return NULL;
	}
	i = (len + 1) / 41 - 1;
	graft = xmalloc(sizeof(*graft) + 20 * i);
	graft->nr_parent = i;
	if (get_sha1_hex(buf, graft->sha1))
		goto bad_graft_data;
	for (i = 40; i < len; i += 41) {
		if (buf[i] != ' ')
			goto bad_graft_data;
		if (get_sha1_hex(buf + i + 1, graft->parent[i/41]))
			goto bad_graft_data;
	}
	return graft;
}

int read_graft_file(const char *graft_file)
{
	FILE *fp = fopen(graft_file, "r");
	char buf[1024];
	if (!fp)
		return -1;
	while (fgets(buf, sizeof(buf), fp)) {
		/* The format is just "Commit Parent1 Parent2 ...\n" */
		int len = strlen(buf);
		struct commit_graft *graft = read_graft_line(buf, len);
		if (!graft)
			continue;
		if (register_commit_graft(graft, 1))
			error("duplicate graft data: %s", buf);
	}
	fclose(fp);
	return 0;
}

static void prepare_commit_graft(void)
{
	static int commit_graft_prepared;
	char *graft_file;

	if (commit_graft_prepared)
		return;
	graft_file = get_graft_file();
	read_graft_file(graft_file);
	/* make sure shallows are read */
	is_repository_shallow();
	commit_graft_prepared = 1;
}

static struct commit_graft *lookup_commit_graft(const unsigned char *sha1)
{
	int pos;
	prepare_commit_graft();
	pos = commit_graft_pos(sha1);
	if (pos < 0)
		return NULL;
	return commit_graft[pos];
}

int write_shallow_commits(int fd, int use_pack_protocol)
{
	int i, count = 0;
	for (i = 0; i < commit_graft_nr; i++)
		if (commit_graft[i]->nr_parent < 0) {
			const char *hex =
				sha1_to_hex(commit_graft[i]->sha1);
			count++;
			if (use_pack_protocol)
				packet_write(fd, "shallow %s", hex);
			else {
				if (write_in_full(fd, hex,  40) != 40)
					break;
				if (write_in_full(fd, "\n", 1) != 1)
					break;
			}
		}
	return count;
}

int unregister_shallow(const unsigned char *sha1)
{
	int pos = commit_graft_pos(sha1);
	if (pos < 0)
		return -1;
	if (pos + 1 < commit_graft_nr)
		memcpy(commit_graft + pos, commit_graft + pos + 1,
				sizeof(struct commit_graft *)
				* (commit_graft_nr - pos - 1));
	commit_graft_nr--;
	return 0;
}

int parse_commit_buffer(struct commit *item, void *buffer, unsigned long size)
{
	char *tail = buffer;
	char *bufptr = buffer;
	unsigned char parent[20];
	struct commit_list **pptr;
	struct commit_graft *graft;
	unsigned n_refs = 0;

	if (item->object.parsed)
		return 0;
	item->object.parsed = 1;
	tail += size;
	if (tail <= bufptr + 5 || memcmp(bufptr, "tree ", 5))
		return error("bogus commit object %s", sha1_to_hex(item->object.sha1));
	if (tail <= bufptr + 45 || get_sha1_hex(bufptr + 5, parent) < 0)
		return error("bad tree pointer in commit %s",
			     sha1_to_hex(item->object.sha1));
	item->tree = lookup_tree(parent);
	if (item->tree)
		n_refs++;
	bufptr += 46; /* "tree " + "hex sha1" + "\n" */
	pptr = &item->parents;

	graft = lookup_commit_graft(item->object.sha1);
	while (bufptr + 48 < tail && !memcmp(bufptr, "parent ", 7)) {
		struct commit *new_parent;

		if (tail <= bufptr + 48 ||
		    get_sha1_hex(bufptr + 7, parent) ||
		    bufptr[47] != '\n')
			return error("bad parents in commit %s", sha1_to_hex(item->object.sha1));
		bufptr += 48;
		if (graft)
			continue;
		new_parent = lookup_commit(parent);
		if (new_parent) {
			pptr = &commit_list_insert(new_parent, pptr)->next;
			n_refs++;
		}
	}
	if (graft) {
		int i;
		struct commit *new_parent;
		for (i = 0; i < graft->nr_parent; i++) {
			new_parent = lookup_commit(graft->parent[i]);
			if (!new_parent)
				continue;
			pptr = &commit_list_insert(new_parent, pptr)->next;
			n_refs++;
		}
	}
	item->date = parse_commit_date(bufptr);

	if (track_object_refs) {
		unsigned i = 0;
		struct commit_list *p;
		struct object_refs *refs = alloc_object_refs(n_refs);
		if (item->tree)
			refs->ref[i++] = &item->tree->object;
		for (p = item->parents; p; p = p->next)
			refs->ref[i++] = &p->item->object;
		set_object_refs(&item->object, refs);
	}

	return 0;
}

int parse_commit(struct commit *item)
{
	enum object_type type;
	void *buffer;
	unsigned long size;
	int ret;

	if (item->object.parsed)
		return 0;
	buffer = read_sha1_file(item->object.sha1, &type, &size);
	if (!buffer)
		return error("Could not read %s",
			     sha1_to_hex(item->object.sha1));
	if (type != OBJ_COMMIT) {
		free(buffer);
		return error("Object %s not a commit",
			     sha1_to_hex(item->object.sha1));
	}
	ret = parse_commit_buffer(item, buffer, size);
	if (save_commit_buffer && !ret) {
		item->buffer = buffer;
		return 0;
	}
	free(buffer);
	return ret;
}

struct commit_list *commit_list_insert(struct commit *item, struct commit_list **list_p)
{
	struct commit_list *new_list = xmalloc(sizeof(struct commit_list));
	new_list->item = item;
	new_list->next = *list_p;
	*list_p = new_list;
	return new_list;
}

void free_commit_list(struct commit_list *list)
{
	while (list) {
		struct commit_list *temp = list;
		list = temp->next;
		free(temp);
	}
}

struct commit_list * insert_by_date(struct commit *item, struct commit_list **list)
{
	struct commit_list **pp = list;
	struct commit_list *p;
	while ((p = *pp) != NULL) {
		if (p->item->date < item->date) {
			break;
		}
		pp = &p->next;
	}
	return commit_list_insert(item, pp);
}


void sort_by_date(struct commit_list **list)
{
	struct commit_list *ret = NULL;
	while (*list) {
		insert_by_date((*list)->item, &ret);
		*list = (*list)->next;
	}
	*list = ret;
}

struct commit *pop_most_recent_commit(struct commit_list **list,
				      unsigned int mark)
{
	struct commit *ret = (*list)->item;
	struct commit_list *parents = ret->parents;
	struct commit_list *old = *list;

	*list = (*list)->next;
	free(old);

	while (parents) {
		struct commit *commit = parents->item;
		parse_commit(commit);
		if (!(commit->object.flags & mark)) {
			commit->object.flags |= mark;
			insert_by_date(commit, list);
		}
		parents = parents->next;
	}
	return ret;
}

void clear_commit_marks(struct commit *commit, unsigned int mark)
{
	while (commit) {
		struct commit_list *parents;

		if (!(mark & commit->object.flags))
			return;

		commit->object.flags &= ~mark;

		parents = commit->parents;
		if (!parents)
			return;

		while ((parents = parents->next))
			clear_commit_marks(parents->item, mark);

		commit = commit->parents->item;
	}
}

/*
 * Generic support for pretty-printing the header
 */
static int get_one_line(const char *msg)
{
	int ret = 0;

	for (;;) {
		char c = *msg++;
		if (!c)
			break;
		ret++;
		if (c == '\n')
			break;
	}
	return ret;
}

/* High bit set, or ISO-2022-INT */
int non_ascii(int ch)
{
	ch = (ch & 0xff);
	return ((ch & 0x80) || (ch == 0x1b));
}

static int is_rfc2047_special(char ch)
{
	return (non_ascii(ch) || (ch == '=') || (ch == '?') || (ch == '_'));
}

static void add_rfc2047(struct strbuf *sb, const char *line, int len,
		       const char *encoding)
{
	int i, last;

	for (i = 0; i < len; i++) {
		int ch = line[i];
		if (non_ascii(ch))
			goto needquote;
		if ((i + 1 < len) && (ch == '=' && line[i+1] == '?'))
			goto needquote;
	}
	strbuf_add(sb, line, len);
	return;

needquote:
	strbuf_grow(sb, len * 3 + strlen(encoding) + 100);
	strbuf_addf(sb, "=?%s?q?", encoding);
	for (i = last = 0; i < len; i++) {
		unsigned ch = line[i] & 0xFF;
		/*
		 * We encode ' ' using '=20' even though rfc2047
		 * allows using '_' for readability.  Unfortunately,
		 * many programs do not understand this and just
		 * leave the underscore in place.
		 */
		if (is_rfc2047_special(ch) || ch == ' ') {
			strbuf_add(sb, line + last, i - last);
			strbuf_addf(sb, "=%02X", ch);
			last = i + 1;
		}
	}
	strbuf_add(sb, line + last, len - last);
	strbuf_addstr(sb, "?=");
}

static void add_user_info(const char *what, enum cmit_fmt fmt, struct strbuf *sb,
			 const char *line, enum date_mode dmode,
			 const char *encoding)
{
	char *date;
	int namelen;
	unsigned long time;
	int tz;
	const char *filler = "    ";

	if (fmt == CMIT_FMT_ONELINE)
		return;
	date = strchr(line, '>');
	if (!date)
		return;
	namelen = ++date - line;
	time = strtoul(date, &date, 10);
	tz = strtol(date, NULL, 10);

	if (fmt == CMIT_FMT_EMAIL) {
		char *name_tail = strchr(line, '<');
		int display_name_length;
		if (!name_tail)
			return;
		while (line < name_tail && isspace(name_tail[-1]))
			name_tail--;
		display_name_length = name_tail - line;
		filler = "";
		strbuf_addstr(sb, "From: ");
		add_rfc2047(sb, line, display_name_length, encoding);
		strbuf_add(sb, name_tail, namelen - display_name_length);
		strbuf_addch(sb, '\n');
	} else {
		strbuf_addf(sb, "%s: %.*s%.*s\n", what,
			      (fmt == CMIT_FMT_FULLER) ? 4 : 0,
			      filler, namelen, line);
	}
	switch (fmt) {
	case CMIT_FMT_MEDIUM:
		strbuf_addf(sb, "Date:   %s\n", show_date(time, tz, dmode));
		break;
	case CMIT_FMT_EMAIL:
		strbuf_addf(sb, "Date: %s\n", show_date(time, tz, DATE_RFC2822));
		break;
	case CMIT_FMT_FULLER:
		strbuf_addf(sb, "%sDate: %s\n", what, show_date(time, tz, dmode));
		break;
	default:
		/* notin' */
		break;
	}
}

static int is_empty_line(const char *line, int *len_p)
{
	int len = *len_p;
	while (len && isspace(line[len-1]))
		len--;
	*len_p = len;
	return !len;
}

static void add_merge_info(enum cmit_fmt fmt, struct strbuf *sb,
			const struct commit *commit, int abbrev)
{
	struct commit_list *parent = commit->parents;

	if ((fmt == CMIT_FMT_ONELINE) || (fmt == CMIT_FMT_EMAIL) ||
	    !parent || !parent->next)
		return;

	strbuf_addstr(sb, "Merge:");

	while (parent) {
		struct commit *p = parent->item;
		const char *hex = NULL;
		const char *dots;
		if (abbrev)
			hex = find_unique_abbrev(p->object.sha1, abbrev);
		if (!hex)
			hex = sha1_to_hex(p->object.sha1);
		dots = (abbrev && strlen(hex) != 40) ?  "..." : "";
		parent = parent->next;

		strbuf_addf(sb, " %s%s", hex, dots);
	}
	strbuf_addch(sb, '\n');
}

static char *get_header(const struct commit *commit, const char *key)
{
	int key_len = strlen(key);
	const char *line = commit->buffer;

	for (;;) {
		const char *eol = strchr(line, '\n'), *next;

		if (line == eol)
			return NULL;
		if (!eol) {
			eol = line + strlen(line);
			next = NULL;
		} else
			next = eol + 1;
		if (eol - line > key_len &&
		    !strncmp(line, key, key_len) &&
		    line[key_len] == ' ') {
			return xmemdupz(line + key_len + 1, eol - line - key_len - 1);
		}
		line = next;
	}
}

static char *replace_encoding_header(char *buf, const char *encoding)
{
	struct strbuf tmp;
	size_t start, len;
	char *cp = buf;

	/* guess if there is an encoding header before a \n\n */
	while (strncmp(cp, "encoding ", strlen("encoding "))) {
		cp = strchr(cp, '\n');
		if (!cp || *++cp == '\n')
			return buf;
	}
	start = cp - buf;
	cp = strchr(cp, '\n');
	if (!cp)
		return buf; /* should not happen but be defensive */
	len = cp + 1 - (buf + start);

	strbuf_init(&tmp, 0);
	strbuf_attach(&tmp, buf, strlen(buf), strlen(buf) + 1);
	if (is_encoding_utf8(encoding)) {
		/* we have re-coded to UTF-8; drop the header */
		strbuf_remove(&tmp, start, len);
	} else {
		/* just replaces XXXX in 'encoding XXXX\n' */
		strbuf_splice(&tmp, start + strlen("encoding "),
					  len - strlen("encoding \n"),
					  encoding, strlen(encoding));
	}
	return strbuf_detach(&tmp, NULL);
}

static char *logmsg_reencode(const struct commit *commit,
			     const char *output_encoding)
{
	static const char *utf8 = "utf-8";
	const char *use_encoding;
	char *encoding;
	char *out;

	if (!*output_encoding)
		return NULL;
	encoding = get_header(commit, "encoding");
	use_encoding = encoding ? encoding : utf8;
	if (!strcmp(use_encoding, output_encoding))
		if (encoding) /* we'll strip encoding header later */
			out = xstrdup(commit->buffer);
		else
			return NULL; /* nothing to do */
	else
		out = reencode_string(commit->buffer,
				      output_encoding, use_encoding);
	if (out)
		out = replace_encoding_header(out, output_encoding);

	free(encoding);
	return out;
}

static void fill_person(struct interp *table, const char *msg, int len)
{
	int start, end, tz = 0;
	unsigned long date;
	char *ep;

	/* parse name */
	for (end = 0; end < len && msg[end] != '<'; end++)
		; /* do nothing */
	start = end + 1;
	while (end > 0 && isspace(msg[end - 1]))
		end--;
	table[0].value = xmemdupz(msg, end);

	if (start >= len)
		return;

	/* parse email */
	for (end = start + 1; end < len && msg[end] != '>'; end++)
		; /* do nothing */

	if (end >= len)
		return;

	table[1].value = xmemdupz(msg + start, end - start);

	/* parse date */
	for (start = end + 1; start < len && isspace(msg[start]); start++)
		; /* do nothing */
	if (start >= len)
		return;
	date = strtoul(msg + start, &ep, 10);
	if (msg + start == ep)
		return;

	table[5].value = xmemdupz(msg + start, ep - (msg + start));

	/* parse tz */
	for (start = ep - msg + 1; start < len && isspace(msg[start]); start++)
		; /* do nothing */
	if (start + 1 < len) {
		tz = strtoul(msg + start + 1, NULL, 10);
		if (msg[start] == '-')
			tz = -tz;
	}

	interp_set_entry(table, 2, show_date(date, tz, DATE_NORMAL));
	interp_set_entry(table, 3, show_date(date, tz, DATE_RFC2822));
	interp_set_entry(table, 4, show_date(date, tz, DATE_RELATIVE));
	interp_set_entry(table, 6, show_date(date, tz, DATE_ISO8601));
}

void format_commit_message(const struct commit *commit,
                           const void *format, struct strbuf *sb)
{
	struct interp table[] = {
		{ "%H" },	/* commit hash */
		{ "%h" },	/* abbreviated commit hash */
		{ "%T" },	/* tree hash */
		{ "%t" },	/* abbreviated tree hash */
		{ "%P" },	/* parent hashes */
		{ "%p" },	/* abbreviated parent hashes */
		{ "%an" },	/* author name */
		{ "%ae" },	/* author email */
		{ "%ad" },	/* author date */
		{ "%aD" },	/* author date, RFC2822 style */
		{ "%ar" },	/* author date, relative */
		{ "%at" },	/* author date, UNIX timestamp */
		{ "%ai" },	/* author date, ISO 8601 */
		{ "%cn" },	/* committer name */
		{ "%ce" },	/* committer email */
		{ "%cd" },	/* committer date */
		{ "%cD" },	/* committer date, RFC2822 style */
		{ "%cr" },	/* committer date, relative */
		{ "%ct" },	/* committer date, UNIX timestamp */
		{ "%ci" },	/* committer date, ISO 8601 */
		{ "%e" },	/* encoding */
		{ "%s" },	/* subject */
		{ "%b" },	/* body */
		{ "%Cred" },	/* red */
		{ "%Cgreen" },	/* green */
		{ "%Cblue" },	/* blue */
		{ "%Creset" },	/* reset color */
		{ "%n" },	/* newline */
		{ "%m" },	/* left/right/bottom */
	};
	enum interp_index {
		IHASH = 0, IHASH_ABBREV,
		ITREE, ITREE_ABBREV,
		IPARENTS, IPARENTS_ABBREV,
		IAUTHOR_NAME, IAUTHOR_EMAIL,
		IAUTHOR_DATE, IAUTHOR_DATE_RFC2822, IAUTHOR_DATE_RELATIVE,
		IAUTHOR_TIMESTAMP, IAUTHOR_ISO8601,
		ICOMMITTER_NAME, ICOMMITTER_EMAIL,
		ICOMMITTER_DATE, ICOMMITTER_DATE_RFC2822,
		ICOMMITTER_DATE_RELATIVE, ICOMMITTER_TIMESTAMP,
		ICOMMITTER_ISO8601,
		IENCODING,
		ISUBJECT,
		IBODY,
		IRED, IGREEN, IBLUE, IRESET_COLOR,
		INEWLINE,
		ILEFT_RIGHT,
	};
	struct commit_list *p;
	char parents[1024];
	unsigned long len;
	int i;
	enum { HEADER, SUBJECT, BODY } state;
	const char *msg = commit->buffer;

	if (ILEFT_RIGHT + 1 != ARRAY_SIZE(table))
		die("invalid interp table!");

	/* these are independent of the commit */
	interp_set_entry(table, IRED, "\033[31m");
	interp_set_entry(table, IGREEN, "\033[32m");
	interp_set_entry(table, IBLUE, "\033[34m");
	interp_set_entry(table, IRESET_COLOR, "\033[m");
	interp_set_entry(table, INEWLINE, "\n");

	/* these depend on the commit */
	if (!commit->object.parsed)
		parse_object(commit->object.sha1);
	interp_set_entry(table, IHASH, sha1_to_hex(commit->object.sha1));
	interp_set_entry(table, IHASH_ABBREV,
			find_unique_abbrev(commit->object.sha1,
				DEFAULT_ABBREV));
	interp_set_entry(table, ITREE, sha1_to_hex(commit->tree->object.sha1));
	interp_set_entry(table, ITREE_ABBREV,
			find_unique_abbrev(commit->tree->object.sha1,
				DEFAULT_ABBREV));
	interp_set_entry(table, ILEFT_RIGHT,
			 (commit->object.flags & BOUNDARY)
			 ? "-"
			 : (commit->object.flags & SYMMETRIC_LEFT)
			 ? "<"
			 : ">");

	parents[1] = 0;
	for (i = 0, p = commit->parents;
			p && i < sizeof(parents) - 1;
			p = p->next)
		i += snprintf(parents + i, sizeof(parents) - i - 1, " %s",
			sha1_to_hex(p->item->object.sha1));
	interp_set_entry(table, IPARENTS, parents + 1);

	parents[1] = 0;
	for (i = 0, p = commit->parents;
			p && i < sizeof(parents) - 1;
			p = p->next)
		i += snprintf(parents + i, sizeof(parents) - i - 1, " %s",
			find_unique_abbrev(p->item->object.sha1,
				DEFAULT_ABBREV));
	interp_set_entry(table, IPARENTS_ABBREV, parents + 1);

	for (i = 0, state = HEADER; msg[i] && state < BODY; i++) {
		int eol;
		for (eol = i; msg[eol] && msg[eol] != '\n'; eol++)
			; /* do nothing */

		if (state == SUBJECT) {
			table[ISUBJECT].value = xmemdupz(msg + i, eol - i);
			i = eol;
		}
		if (i == eol) {
			state++;
			/* strip empty lines */
			while (msg[eol + 1] == '\n')
				eol++;
		} else if (!prefixcmp(msg + i, "author "))
			fill_person(table + IAUTHOR_NAME,
					msg + i + 7, eol - i - 7);
		else if (!prefixcmp(msg + i, "committer "))
			fill_person(table + ICOMMITTER_NAME,
					msg + i + 10, eol - i - 10);
		else if (!prefixcmp(msg + i, "encoding "))
			table[IENCODING].value =
				xmemdupz(msg + i + 9, eol - i - 9);
		i = eol;
	}
	if (msg[i])
		table[IBODY].value = xstrdup(msg + i);

	len = interpolate(sb->buf + sb->len, strbuf_avail(sb),
				format, table, ARRAY_SIZE(table));
	if (len > strbuf_avail(sb)) {
		strbuf_grow(sb, len);
		interpolate(sb->buf + sb->len, strbuf_avail(sb) + 1,
					format, table, ARRAY_SIZE(table));
	}
	strbuf_setlen(sb, sb->len + len);
	interp_clear_table(table, ARRAY_SIZE(table));
}

static void pp_header(enum cmit_fmt fmt,
		      int abbrev,
		      enum date_mode dmode,
		      const char *encoding,
		      const struct commit *commit,
		      const char **msg_p,
		      struct strbuf *sb)
{
	int parents_shown = 0;

	for (;;) {
		const char *line = *msg_p;
		int linelen = get_one_line(*msg_p);

		if (!linelen)
			return;
		*msg_p += linelen;

		if (linelen == 1)
			/* End of header */
			return;

		if (fmt == CMIT_FMT_RAW) {
			strbuf_add(sb, line, linelen);
			continue;
		}

		if (!memcmp(line, "parent ", 7)) {
			if (linelen != 48)
				die("bad parent line in commit");
			continue;
		}

		if (!parents_shown) {
			struct commit_list *parent;
			int num;
			for (parent = commit->parents, num = 0;
			     parent;
			     parent = parent->next, num++)
				;
			/* with enough slop */
			strbuf_grow(sb, num * 50 + 20);
			add_merge_info(fmt, sb, commit, abbrev);
			parents_shown = 1;
		}

		/*
		 * MEDIUM == DEFAULT shows only author with dates.
		 * FULL shows both authors but not dates.
		 * FULLER shows both authors and dates.
		 */
		if (!memcmp(line, "author ", 7)) {
			strbuf_grow(sb, linelen + 80);
			add_user_info("Author", fmt, sb, line + 7, dmode, encoding);
		}
		if (!memcmp(line, "committer ", 10) &&
		    (fmt == CMIT_FMT_FULL || fmt == CMIT_FMT_FULLER)) {
			strbuf_grow(sb, linelen + 80);
			add_user_info("Commit", fmt, sb, line + 10, dmode, encoding);
		}
	}
}

static void pp_title_line(enum cmit_fmt fmt,
			  const char **msg_p,
			  struct strbuf *sb,
			  const char *subject,
			  const char *after_subject,
			  const char *encoding,
			  int plain_non_ascii)
{
	struct strbuf title;

	strbuf_init(&title, 80);

	for (;;) {
		const char *line = *msg_p;
		int linelen = get_one_line(line);

		*msg_p += linelen;
		if (!linelen || is_empty_line(line, &linelen))
			break;

		strbuf_grow(&title, linelen + 2);
		if (title.len) {
			if (fmt == CMIT_FMT_EMAIL) {
				strbuf_addch(&title, '\n');
			}
			strbuf_addch(&title, ' ');
		}
		strbuf_add(&title, line, linelen);
	}

	strbuf_grow(sb, title.len + 1024);
	if (subject) {
		strbuf_addstr(sb, subject);
		add_rfc2047(sb, title.buf, title.len, encoding);
	} else {
		strbuf_addbuf(sb, &title);
	}
	strbuf_addch(sb, '\n');

	if (plain_non_ascii) {
		const char *header_fmt =
			"MIME-Version: 1.0\n"
			"Content-Type: text/plain; charset=%s\n"
			"Content-Transfer-Encoding: 8bit\n";
		strbuf_addf(sb, header_fmt, encoding);
	}
	if (after_subject) {
		strbuf_addstr(sb, after_subject);
	}
	if (fmt == CMIT_FMT_EMAIL) {
		strbuf_addch(sb, '\n');
	}
	strbuf_release(&title);
}

static void pp_remainder(enum cmit_fmt fmt,
			 const char **msg_p,
			 struct strbuf *sb,
			 int indent)
{
	int first = 1;
	for (;;) {
		const char *line = *msg_p;
		int linelen = get_one_line(line);
		*msg_p += linelen;

		if (!linelen)
			break;

		if (is_empty_line(line, &linelen)) {
			if (first)
				continue;
			if (fmt == CMIT_FMT_SHORT)
				break;
		}
		first = 0;

		strbuf_grow(sb, linelen + indent + 20);
		if (indent) {
			memset(sb->buf + sb->len, ' ', indent);
			strbuf_setlen(sb, sb->len + indent);
		}
		strbuf_add(sb, line, linelen);
		strbuf_addch(sb, '\n');
	}
}

void pretty_print_commit(enum cmit_fmt fmt, const struct commit *commit,
				  struct strbuf *sb, int abbrev,
				  const char *subject, const char *after_subject,
				  enum date_mode dmode, int plain_non_ascii)
{
	unsigned long beginning_of_body;
	int indent = 4;
	const char *msg = commit->buffer;
	char *reencoded;
	const char *encoding;

	if (fmt == CMIT_FMT_USERFORMAT) {
		format_commit_message(commit, user_format, sb);
		return;
	}

	encoding = (git_log_output_encoding
		    ? git_log_output_encoding
		    : git_commit_encoding);
	if (!encoding)
		encoding = "utf-8";
	reencoded = logmsg_reencode(commit, encoding);
	if (reencoded) {
		msg = reencoded;
	}

	if (fmt == CMIT_FMT_ONELINE || fmt == CMIT_FMT_EMAIL)
		indent = 0;

	/* After-subject is used to pass in Content-Type: multipart
	 * MIME header; in that case we do not have to do the
	 * plaintext content type even if the commit message has
	 * non 7-bit ASCII character.  Otherwise, check if we need
	 * to say this is not a 7-bit ASCII.
	 */
	if (fmt == CMIT_FMT_EMAIL && !after_subject) {
		int i, ch, in_body;

		for (in_body = i = 0; (ch = msg[i]); i++) {
			if (!in_body) {
				/* author could be non 7-bit ASCII but
				 * the log may be so; skip over the
				 * header part first.
				 */
				if (ch == '\n' && msg[i+1] == '\n')
					in_body = 1;
			}
			else if (non_ascii(ch)) {
				plain_non_ascii = 1;
				break;
			}
		}
	}

	pp_header(fmt, abbrev, dmode, encoding, commit, &msg, sb);
	if (fmt != CMIT_FMT_ONELINE && !subject) {
		strbuf_addch(sb, '\n');
	}

	/* Skip excess blank lines at the beginning of body, if any... */
	for (;;) {
		int linelen = get_one_line(msg);
		int ll = linelen;
		if (!linelen)
			break;
		if (!is_empty_line(msg, &ll))
			break;
		msg += linelen;
	}

	/* These formats treat the title line specially. */
	if (fmt == CMIT_FMT_ONELINE || fmt == CMIT_FMT_EMAIL)
		pp_title_line(fmt, &msg, sb, subject,
			      after_subject, encoding, plain_non_ascii);

	beginning_of_body = sb->len;
	if (fmt != CMIT_FMT_ONELINE)
		pp_remainder(fmt, &msg, sb, indent);
	strbuf_rtrim(sb);

	/* Make sure there is an EOLN for the non-oneline case */
	if (fmt != CMIT_FMT_ONELINE)
		strbuf_addch(sb, '\n');

	/*
	 * The caller may append additional body text in e-mail
	 * format.  Make sure we did not strip the blank line
	 * between the header and the body.
	 */
	if (fmt == CMIT_FMT_EMAIL && sb->len <= beginning_of_body)
		strbuf_addch(sb, '\n');
	free(reencoded);
}

struct commit *pop_commit(struct commit_list **stack)
{
	struct commit_list *top = *stack;
	struct commit *item = top ? top->item : NULL;

	if (top) {
		*stack = top->next;
		free(top);
	}
	return item;
}

/*
 * Performs an in-place topological sort on the list supplied.
 */
void sort_in_topological_order(struct commit_list ** list, int lifo)
{
	struct commit_list *next, *orig = *list;
	struct commit_list *work, **insert;
	struct commit_list **pptr;

	if (!orig)
		return;
	*list = NULL;

	/* Mark them and clear the indegree */
	for (next = orig; next; next = next->next) {
		struct commit *commit = next->item;
		commit->object.flags |= TOPOSORT;
		commit->indegree = 0;
	}

	/* update the indegree */
	for (next = orig; next; next = next->next) {
		struct commit_list * parents = next->item->parents;
		while (parents) {
			struct commit *parent = parents->item;

			if (parent->object.flags & TOPOSORT)
				parent->indegree++;
			parents = parents->next;
		}
	}

	/*
	 * find the tips
	 *
	 * tips are nodes not reachable from any other node in the list
	 *
	 * the tips serve as a starting set for the work queue.
	 */
	work = NULL;
	insert = &work;
	for (next = orig; next; next = next->next) {
		struct commit *commit = next->item;

		if (!commit->indegree)
			insert = &commit_list_insert(commit, insert)->next;
	}

	/* process the list in topological order */
	if (!lifo)
		sort_by_date(&work);

	pptr = list;
	*list = NULL;
	while (work) {
		struct commit *commit;
		struct commit_list *parents, *work_item;

		work_item = work;
		work = work_item->next;
		work_item->next = NULL;

		commit = work_item->item;
		for (parents = commit->parents; parents ; parents = parents->next) {
			struct commit *parent=parents->item;

			if (!(parent->object.flags & TOPOSORT))
				continue;

			/*
			 * parents are only enqueued for emission
			 * when all their children have been emitted thereby
			 * guaranteeing topological order.
			 */
			if (!--parent->indegree) {
				if (!lifo)
					insert_by_date(parent, &work);
				else
					commit_list_insert(parent, &work);
			}
		}
		/*
		 * work_item is a commit all of whose children
		 * have already been emitted. we can emit it now.
		 */
		commit->object.flags &= ~TOPOSORT;
		*pptr = work_item;
		pptr = &work_item->next;
	}
}

/* merge-base stuff */

/* bits #0..15 in revision.h */
#define PARENT1		(1u<<16)
#define PARENT2		(1u<<17)
#define STALE		(1u<<18)
#define RESULT		(1u<<19)

static const unsigned all_flags = (PARENT1 | PARENT2 | STALE | RESULT);

static struct commit *interesting(struct commit_list *list)
{
	while (list) {
		struct commit *commit = list->item;
		list = list->next;
		if (commit->object.flags & STALE)
			continue;
		return commit;
	}
	return NULL;
}

static struct commit_list *merge_bases(struct commit *one, struct commit *two)
{
	struct commit_list *list = NULL;
	struct commit_list *result = NULL;

	if (one == two)
		/* We do not mark this even with RESULT so we do not
		 * have to clean it up.
		 */
		return commit_list_insert(one, &result);

	parse_commit(one);
	parse_commit(two);

	one->object.flags |= PARENT1;
	two->object.flags |= PARENT2;
	insert_by_date(one, &list);
	insert_by_date(two, &list);

	while (interesting(list)) {
		struct commit *commit;
		struct commit_list *parents;
		struct commit_list *n;
		int flags;

		commit = list->item;
		n = list->next;
		free(list);
		list = n;

		flags = commit->object.flags & (PARENT1 | PARENT2 | STALE);
		if (flags == (PARENT1 | PARENT2)) {
			if (!(commit->object.flags & RESULT)) {
				commit->object.flags |= RESULT;
				insert_by_date(commit, &result);
			}
			/* Mark parents of a found merge stale */
			flags |= STALE;
		}
		parents = commit->parents;
		while (parents) {
			struct commit *p = parents->item;
			parents = parents->next;
			if ((p->object.flags & flags) == flags)
				continue;
			parse_commit(p);
			p->object.flags |= flags;
			insert_by_date(p, &list);
		}
	}

	/* Clean up the result to remove stale ones */
	free_commit_list(list);
	list = result; result = NULL;
	while (list) {
		struct commit_list *n = list->next;
		if (!(list->item->object.flags & STALE))
			insert_by_date(list->item, &result);
		free(list);
		list = n;
	}
	return result;
}

struct commit_list *get_merge_bases(struct commit *one,
					struct commit *two, int cleanup)
{
	struct commit_list *list;
	struct commit **rslt;
	struct commit_list *result;
	int cnt, i, j;

	result = merge_bases(one, two);
	if (one == two)
		return result;
	if (!result || !result->next) {
		if (cleanup) {
			clear_commit_marks(one, all_flags);
			clear_commit_marks(two, all_flags);
		}
		return result;
	}

	/* There are more than one */
	cnt = 0;
	list = result;
	while (list) {
		list = list->next;
		cnt++;
	}
	rslt = xcalloc(cnt, sizeof(*rslt));
	for (list = result, i = 0; list; list = list->next)
		rslt[i++] = list->item;
	free_commit_list(result);

	clear_commit_marks(one, all_flags);
	clear_commit_marks(two, all_flags);
	for (i = 0; i < cnt - 1; i++) {
		for (j = i+1; j < cnt; j++) {
			if (!rslt[i] || !rslt[j])
				continue;
			result = merge_bases(rslt[i], rslt[j]);
			clear_commit_marks(rslt[i], all_flags);
			clear_commit_marks(rslt[j], all_flags);
			for (list = result; list; list = list->next) {
				if (rslt[i] == list->item)
					rslt[i] = NULL;
				if (rslt[j] == list->item)
					rslt[j] = NULL;
			}
		}
	}

	/* Surviving ones in rslt[] are the independent results */
	result = NULL;
	for (i = 0; i < cnt; i++) {
		if (rslt[i])
			insert_by_date(rslt[i], &result);
	}
	free(rslt);
	return result;
}

int in_merge_bases(struct commit *commit, struct commit **reference, int num)
{
	struct commit_list *bases, *b;
	int ret = 0;

	if (num == 1)
		bases = get_merge_bases(commit, *reference, 1);
	else
		die("not yet");
	for (b = bases; b; b = b->next) {
		if (!hashcmp(commit->object.sha1, b->item->object.sha1)) {
			ret = 1;
			break;
		}
	}

	free_commit_list(bases);
	return ret;
}
