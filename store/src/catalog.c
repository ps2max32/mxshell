/* The index (apps/mxstore docs/index-format.md): index.ini, signed by the
   catalogue's CI with ECDSA P-256 over its SHA-256 (index.sig: r and s, 32
   bytes each). The public key is index_key.pem (roots.S). Nothing of an
   index whose signature does not match is read. */
#include "store.h"

#include <mbedtls/ecdsa.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mx/mx.h>
#include <stdlib.h>
#include <string.h>

extern const char mxstore_index_key[];
extern const u32 mxstore_index_key_size;

struct entry entries[MAX_ENTRIES];
int nentries;
char index_generated[24], index_base[160];
const char *catalog_error = "";

u32 title_colour(const char *t) {
  u32 h = 2166136261u;
  for (; *t; t++)
    h = (h ^ (u8)*t) * 16777619u;
  static const u32 tones[8] = {0x33271F, 0x351C19, 0x2A281C, 0x1B232E, 0x23202B, 0x1C2723, 0x29222C, 0x2C251C};
  return tones[h & 7];
}

int hex32(const char *s, u8 out[32]) {
  for (int i = 0; i < 64; i++) {
    int c = s[i], v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    if (v < 0)
      return -1;
    if (i & 1)
      out[i / 2] |= (u8)v;
    else
      out[i / 2] = (u8)(v << 4);
  }
  return s[64] == 0 || s[64] == ' ' || s[64] == ',' ? 0 : -1;
}

static int verify(const u8 *data, u32 n, const u8 *sig) {
  u8 h[32];
  mbedtls_sha256(data, n, h, 0);
  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
  int r = mbedtls_pk_parse_public_key(&pk, (const u8 *)mxstore_index_key, mxstore_index_key_size);
  if (r == 0 && mbedtls_pk_get_type(&pk) != MBEDTLS_PK_ECKEY)
    r = -1;
  if (r == 0) {
    mbedtls_ecp_keypair *ec = mbedtls_pk_ec(pk);
    mbedtls_mpi rr, ss;
    mbedtls_mpi_init(&rr), mbedtls_mpi_init(&ss);
    r = mbedtls_mpi_read_binary(&rr, sig, 32);
    if (!r)
      r = mbedtls_mpi_read_binary(&ss, sig + 32, 32);
    if (!r)
      r = mbedtls_ecdsa_verify(&ec->MBEDTLS_PRIVATE(grp), h, 32, &ec->MBEDTLS_PRIVATE(Q), &rr, &ss);
    mbedtls_mpi_free(&rr), mbedtls_mpi_free(&ss);
  }
  mbedtls_pk_free(&pk);
  return r;
}

const char *kv_get(const struct entry *e, const char *k) {
  for (int i = 0; i < e->nkv; i++)
    if (!strcmp(e->kv[i].k, k))
      return e->kv[i].v;
  return 0;
}

static void pic(struct pic_ref *p, char *v) {
  /* "path sha256" */
  char *sp = strchr(v, ' ');
  if (!sp || hex32(sp + 1, p->sha) < 0) {
    p->path = 0;
    return;
  }
  *sp = 0;
  p->path = v;
}

static int valid_id(const char *s) {
  int n = 0;
  for (; s[n]; n++)
    if (!((s[n] >= 'a' && s[n] <= 'z') || (s[n] >= '0' && s[n] <= '9') || s[n] == '-'))
      return 0;
  return n > 0 && n <= 31;
}

static void entry_done(struct entry *e) {
  static const char empty[] = "";
#define S(f, key) e->f = kv_get(e, key) ? kv_get(e, key) : empty
  S(title, "title"), S(section, "section"), S(type, "type"), S(year, "year");
  S(description, "description"), S(developer, "developer"), S(players, "players");
  S(version, "version"), S(date, "date"), S(url, "url"), S(needs, "needs"), S(needs_text, "needs_text");
  S(source, "source"), S(license, "license");
#undef S
  const char *k = kv_get(e, "kind");
  e->kind = !k ? K_GAME : !strcmp(k, "emulator") ? K_EMULATOR : !strcmp(k, "app") ? K_APP
          : !strcmp(k, "system") ? K_SYSTEM : !strcmp(k, "data") ? K_DATA : K_GAME;
  const char *u = kv_get(e, "update");
  e->update = !u ? U_NONE : !strcmp(u, "kernel") ? U_KERNEL : !strcmp(u, "boot") ? U_BOOT : U_NONE;
  const char *v;
  e->order = (v = kv_get(e, "order")) ? atoi(v) : 50;
  e->size = (v = kv_get(e, "size")) ? (u32)strtoul(v, 0, 10) : 0;
  e->installed = (v = kv_get(e, "installed")) ? (u32)strtoul(v, 0, 10) : 0;
  e->kernel = (v = kv_get(e, "kernel")) ? (u32)atoi(v) : 0;
  e->provides = (v = kv_get(e, "provides")) ? (u32)atoi(v) : 0;
  memset(e->sha, 0, 32);
  if ((v = kv_get(e, "sha256")))
    hex32(v, e->sha);
  /* the pictures' values are cut into path and hash: kv keeps the key,
     an install does not write them */
  for (int i = 0; i < e->nkv; i++) {
    char *val = (char *)e->kv[i].v;
    if (!strcmp(e->kv[i].k, "cover"))
      pic(&e->cover, val);
    else if (!strcmp(e->kv[i].k, "hero"))
      pic(&e->hero, val);
    else if (!strcmp(e->kv[i].k, "trailer"))
      pic(&e->trailer, val);
    else if (!strcmp(e->kv[i].k, "shots")) {
      e->nshots = 0;
      for (char *s = val; *s && e->nshots < MAX_SHOTS;) {
        char *c = strchr(s, ',');
        if (c)
          *c = 0;
        pic(&e->shots[e->nshots], s);
        if (e->shots[e->nshots].path)
          e->nshots++;
        if (!c)
          break;
        s = c + 1;
      }
    }
  }
  e->base = title_colour(e->title);
  e->on_dev = -1;
}

static void find_versions(struct entry *e);

int catalog_load(char *data, u32 n, const u8 *sig, u32 sig_n) {
  nentries = 0;
  if (sig_n != 64) {
    catalog_error = "The index has no signature";
    return -1;
  }
  if (verify((const u8 *)data, n, sig)) {
    catalog_error = "The index's signature does not match";
    return -1;
  }
  data[n] = 0;
  struct entry *e = 0;
  int in_store = 0, format = 0;
  for (char *s = data; *s;) {
    char *eol = strchr(s, '\n');
    if (eol)
      *eol = 0;
    if (*s == '[') {
      char *end = strchr(s, ']');
      if (end) {
        *end = 0;
        in_store = !strcmp(s + 1, "store");
        e = 0;
        if (!in_store && valid_id(s + 1) && nentries < MAX_ENTRIES) {
          e = &entries[nentries++];
          memset(e, 0, sizeof *e);
          e->id = s + 1;
        }
      }
    } else if (*s && *s != '#' && *s != ';') {
      char *eq = strchr(s, '=');
      if (eq) {
        *eq = 0;
        if (in_store) {
          if (!strcmp(s, "format"))
            format = atoi(eq + 1);
          else if (!strcmp(s, "generated"))
            mx_snprintf(index_generated, sizeof index_generated, "%s", eq + 1);
          else if (!strcmp(s, "base"))
            mx_snprintf(index_base, sizeof index_base, "%s", eq + 1);
        } else if (e && e->nkv < MAX_KV)
          e->kv[e->nkv].k = s, e->kv[e->nkv++].v = eq + 1;
      }
    }
    if (!eol)
      break;
    s = eol + 1;
  }
  if (format != 1) {
    nentries = 0;
    catalog_error = "An index of a newer store: update MXSTORE from the PC";
    return -2;
  }
  int k = 0;
  for (int i = 0; i < nentries; i++) {
    entries[k] = entries[i];
    entry_done(&entries[k]);
    /* the required keys */
    if (*entries[k].title && *entries[k].version && *entries[k].url && entries[k].size)
      k++;
  }
  nentries = k;
  /* by order=, then by id (the index's own order already, kept stable) */
  for (int i = 1; i < nentries; i++)
    for (int j = i; j > 0 && entries[j - 1].order > entries[j].order; j--) {
      struct entry t = entries[j];
      entries[j] = entries[j - 1], entries[j - 1] = t;
    }
  for (int i = 0; i < nentries; i++)
    find_versions(&entries[i]);
  return 0;
}

/* Does data entry d belong to game g: the same data directory (all the
   games that share it show the same data), or its for= names g */
static int data_of(const struct entry *g, const struct entry *d) {
  const char *gd = kv_get(g, "data"), *dd = kv_get(d, "data"), *f = kv_get(d, "for");
  if (gd && dd && !strcmp(gd, dd))
    return 1;
  u32 l = (u32)strlen(g->id);
  for (; f && *f; f = strchr(f, ',') ? strchr(f, ',') + 1 : 0)
    if (!strncmp(f, g->id, l) && (f[l] == ',' || !f[l]))
      return 1;
  return 0;
}
/* the first of a needs= ("a|b": a) */
static void first_need(char *out, u32 n, const char *needs) {
  u32 l = 0;
  while (needs[l] && needs[l] != '|' && l + 1 < n)
    out[l] = needs[l], l++;
  out[l] = 0;
}
/* the data entry of g that brings file, -1 none */
static int data_for(const struct entry *g, const char *file) {
  for (int d = 0; d < nentries; d++)
    if (entries[d].kind == K_DATA && data_of(g, &entries[d])) {
      char f[64];
      first_need(f, sizeof f, entries[d].needs);
      if (!strcmp(f, file))
        return d;
    }
  return -1;
}
/* a game's versions: versions= of its game.ini ("<name>:<file>:<word>",
   "," between them; docs/index-format.md), else its data entries' */
static void find_versions(struct entry *e) {
  e->nvers = 0;
  if (e->kind == K_DATA)
    return;
  const char *v = kv_get(e, "versions");
  if (v) {
    while (*v && e->nvers < MAX_VERS) {
      struct version *ver = &e->vers[e->nvers];
      const char *c1 = strchr(v, ':'), *end = strchr(v, ',');
      if (!end)
        end = v + strlen(v);
      const char *c2 = c1 && c1 < end ? strchr(c1 + 1, ':') : 0;
      if (c1 && c1 < end) {
        const char *fe = c2 && c2 < end ? c2 : end;
        mx_snprintf(ver->name, sizeof ver->name, "%.*s", (int)(c1 - v), v);
        mx_snprintf(ver->file, sizeof ver->file, "%.*s", (int)(fe - c1 - 1), c1 + 1);
        ver->data = data_for(e, ver->file);
        e->nvers++;
      }
      v = *end ? end + 1 : end;
    }
    return;
  }
  for (int d = 0; d < nentries && e->nvers < MAX_VERS; d++)
    if (entries[d].kind == K_DATA && data_of(e, &entries[d])) {
      struct version *ver = &e->vers[e->nvers++];
      mx_snprintf(ver->name, sizeof ver->name, "%s", entries[d].title);
      first_need(ver->file, sizeof ver->file, entries[d].needs);
      ver->data = d;
    }
}
