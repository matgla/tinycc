/*
 *  TCC - Tiny C Compiler
 *
 *  Linker Script Support - Implementation
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "tccld.h"
#include "tcc.h"
#include <ctype.h>
#include <string.h>

/* Token types for linker script lexer */
#define LDTOK_EOF (-1)
#define LDTOK_NAME 256
#define LDTOK_NUM 257
#define LDTOK_STRING 258

/* Parser state */
typedef struct LDParser
{
  TCCState *s1;
  LDScript *ld;
  int fd;          /* file descriptor */
  const char *str; /* string input (if parsing from string) */
  int str_pos;     /* position in string */
  int cc;          /* pushed back character */
  char tok_buf[1024];
  int tok;
  addr_t tok_num;
  int expr_has_loadaddr;
  int expr_loadaddr_section_idx;
} LDParser;

/* ================= Lexer ================= */

static int ld_getc(LDParser *p)
{
  char b;
  if (p->cc != -1)
  {
    int c = p->cc;
    p->cc = -1;
    return c;
  }
  if (p->str)
  {
    if (p->str[p->str_pos] == '\0')
      return EOF;
    return p->str[p->str_pos++];
  }
  if (read(p->fd, &b, 1) == 1)
    return (unsigned char)b;
  return EOF;
}

static void ld_ungetc(LDParser *p, int c)
{
  p->cc = c;
}

static void ld_skip_whitespace(LDParser *p)
{
  int c;
  for (;;)
  {
    c = ld_getc(p);
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v')
      continue;
    if (c == '/')
    {
      int c2 = ld_getc(p);
      if (c2 == '*')
      {
        /* block comment */
        int prev = 0;
        while ((c = ld_getc(p)) != EOF)
        {
          if (c == '/' && prev == '*')
            break;
          prev = c;
        }
        continue;
      }
      else if (c2 == '/')
      {
        /* line comment */
        while ((c = ld_getc(p)) != EOF && c != '\n')
          ;
        continue;
      }
      else
      {
        ld_ungetc(p, c2);
        ld_ungetc(p, c);
        return;
      }
    }
    ld_ungetc(p, c);
    return;
  }
}

static int ld_next_token(LDParser *p)
{
  int c;
  char *q;

  ld_skip_whitespace(p);
  c = ld_getc(p);

  if (c == EOF)
  {
    p->tok = LDTOK_EOF;
    return LDTOK_EOF;
  }

  /* String literal */
  if (c == '"')
  {
    q = p->tok_buf;
    while ((c = ld_getc(p)) != EOF && c != '"')
    {
      if (q - p->tok_buf < (int)sizeof(p->tok_buf) - 1)
        *q++ = c;
    }
    *q = '\0';
    p->tok = LDTOK_STRING;
    return LDTOK_STRING;
  }

  /* Number (hex or decimal) */
  if (isdigit(c) || (c == '0'))
  {
    q = p->tok_buf;
    *q++ = c;
    if (c == '0')
    {
      c = ld_getc(p);
      if (c == 'x' || c == 'X')
      {
        *q++ = c;
        while ((c = ld_getc(p)) != EOF && isxdigit(c))
        {
          if (q - p->tok_buf < (int)sizeof(p->tok_buf) - 1)
            *q++ = c;
        }
        ld_ungetc(p, c);
        *q = '\0';
        p->tok_num = strtoull(p->tok_buf, NULL, 16);
        p->tok = LDTOK_NUM;
        return LDTOK_NUM;
      }
      ld_ungetc(p, c);
    }
    while ((c = ld_getc(p)) != EOF && isdigit(c))
    {
      if (q - p->tok_buf < (int)sizeof(p->tok_buf) - 1)
        *q++ = c;
    }
    /* Check for K, M, G suffixes */
    if (c == 'K' || c == 'k')
    {
      *q = '\0';
      p->tok_num = strtoull(p->tok_buf, NULL, 0) * 1024;
    }
    else if (c == 'M' || c == 'm')
    {
      *q = '\0';
      p->tok_num = strtoull(p->tok_buf, NULL, 0) * 1024 * 1024;
    }
    else if (c == 'G' || c == 'g')
    {
      *q = '\0';
      p->tok_num = strtoull(p->tok_buf, NULL, 0) * 1024 * 1024 * 1024;
    }
    else
    {
      ld_ungetc(p, c);
      *q = '\0';
      p->tok_num = strtoull(p->tok_buf, NULL, 0);
    }
    p->tok = LDTOK_NUM;
    return LDTOK_NUM;
  }

  /* Identifier or keyword */
  if (isalpha(c) || c == '_' || c == '.' || c == '*' || c == '$')
  {
    q = p->tok_buf;
    *q++ = c;
    while ((c = ld_getc(p)) != EOF)
    {
      if (isalnum(c) || c == '_' || c == '.' || c == '*' || c == '$' || c == '-')
      {
        if (q - p->tok_buf < (int)sizeof(p->tok_buf) - 1)
          *q++ = c;
      }
      else
      {
        break;
      }
    }
    ld_ungetc(p, c);
    *q = '\0';
    p->tok = LDTOK_NAME;
    return LDTOK_NAME;
  }

  /* Operators and punctuation */
  p->tok = c;
  p->tok_buf[0] = c;
  p->tok_buf[1] = '\0';

  /* Check for multi-character operators */
  if (c == '>')
  {
    int c2 = ld_getc(p);
    if (c2 == '>')
    {
      p->tok_buf[1] = '>';
      p->tok_buf[2] = '\0';
      return c; /* >> */
    }
    ld_ungetc(p, c2);
  }
  else if (c == '<')
  {
    int c2 = ld_getc(p);
    if (c2 == '<')
    {
      p->tok_buf[1] = '<';
      p->tok_buf[2] = '\0';
      return c; /* << */
    }
    ld_ungetc(p, c2);
  }

  return c;
}

static int ld_expect(LDParser *p, int tok)
{
  TCCState *s1 = p->s1;
  if (p->tok != tok)
  {
    if (tok < 256)
      return tcc_error_noabort("linker script: expected '%c'", tok);
    else
      return tcc_error_noabort("linker script: unexpected token");
  }
  ld_next_token(p);
  return 0;
}

/* ================= Expression Parser ================= */

static addr_t ld_parse_expr(LDParser *p);

static addr_t ld_parse_primary(LDParser *p)
{
  addr_t val = 0;
  addr_t align;
  int idx;

  if (p->tok == LDTOK_NUM)
  {
    val = p->tok_num;
    ld_next_token(p);
  }
  else if (p->tok == '.')
  {
    /* Location counter */
    val = p->ld->location_counter;
    ld_next_token(p);
  }
  else if (p->tok == LDTOK_NAME)
  {
    /* Function call or symbol */
    if (!strcmp(p->tok_buf, "ALIGN"))
    {
      ld_next_token(p);
      ld_expect(p, '(');
      align = ld_parse_expr(p);
      ld_expect(p, ')');
      val = (p->ld->location_counter + align - 1) & ~(align - 1);
    }
    else if (!strcmp(p->tok_buf, "ORIGIN"))
    {
      ld_next_token(p);
      ld_expect(p, '(');
      if (p->tok == LDTOK_NAME)
      {
        idx = ld_script_find_memory_region(p->ld, p->tok_buf);
        if (idx >= 0)
          val = p->ld->memory_regions[idx].origin;
        ld_next_token(p);
      }
      ld_expect(p, ')');
    }
    else if (!strcmp(p->tok_buf, "LENGTH"))
    {
      ld_next_token(p);
      ld_expect(p, '(');
      if (p->tok == LDTOK_NAME)
      {
        idx = ld_script_find_memory_region(p->ld, p->tok_buf);
        if (idx >= 0)
          val = p->ld->memory_regions[idx].length;
        ld_next_token(p);
      }
      ld_expect(p, ')');
    }
    else if (!strcmp(p->tok_buf, "SIZEOF"))
    {
      ld_next_token(p);
      ld_expect(p, '(');
      /* TODO: implement SIZEOF */
      while (p->tok != ')' && p->tok != LDTOK_EOF)
        ld_next_token(p);
      ld_expect(p, ')');
    }
    else if (!strcmp(p->tok_buf, "ADDR"))
    {
      ld_next_token(p);
      ld_expect(p, '(');
      /* TODO: implement ADDR */
      while (p->tok != ')' && p->tok != LDTOK_EOF)
        ld_next_token(p);
      ld_expect(p, ')');
    }
    else if (!strcmp(p->tok_buf, "LOADADDR"))
    {
      ld_next_token(p);
      ld_expect(p, '(');
      if (p->tok == LDTOK_NAME)
      {
        int os_idx = ld_script_find_output_section(p->ld, p->tok_buf);
        if (os_idx >= 0)
        {
          p->expr_has_loadaddr = 1;
          p->expr_loadaddr_section_idx = os_idx;
        }
        ld_next_token(p);
      }
      ld_expect(p, ')');
      /* LOADADDR is resolved after layout; evaluate to 0 for now. */
      val = 0;
    }
    else if (!strcmp(p->tok_buf, "DEFINED"))
    {
      ld_next_token(p);
      ld_expect(p, '(');
      if (p->tok == LDTOK_NAME)
      {
        int idx = ld_script_find_or_create_symbol(p->ld, p->tok_buf);
        val = (idx >= 0 && p->ld->symbols[idx].defined) ? 1 : 0;
        ld_next_token(p);
      }
      ld_expect(p, ')');
    }
    else
    {
      /* Symbol reference */
      int idx = ld_script_find_or_create_symbol(p->ld, p->tok_buf);
      if (idx >= 0 && p->ld->symbols[idx].defined)
        val = p->ld->symbols[idx].value;
      ld_next_token(p);
    }
  }
  else if (p->tok == '(')
  {
    ld_next_token(p);
    val = ld_parse_expr(p);
    ld_expect(p, ')');
  }
  else if (p->tok == '~')
  {
    ld_next_token(p);
    val = ~ld_parse_primary(p);
  }
  else if (p->tok == '-')
  {
    ld_next_token(p);
    val = -ld_parse_primary(p);
  }

  return val;
}

static addr_t ld_parse_mul(LDParser *p)
{
  addr_t val = ld_parse_primary(p);
  while (p->tok == '*' || p->tok == '/' || p->tok == '%')
  {
    int op = p->tok;
    addr_t val2;
    ld_next_token(p);
    val2 = ld_parse_primary(p);
    if (op == '*')
      val *= val2;
    else if (op == '/' && val2)
      val /= val2;
    else if (op == '%' && val2)
      val %= val2;
  }
  return val;
}

static addr_t ld_parse_add(LDParser *p)
{
  addr_t val = ld_parse_mul(p);
  addr_t val2;
  while (p->tok == '+' || p->tok == '-')
  {
    int op = p->tok;
    ld_next_token(p);
    val2 = ld_parse_mul(p);
    if (op == '+')
      val += val2;
    else
      val -= val2;
  }
  return val;
}

static addr_t ld_parse_shift(LDParser *p)
{
  addr_t val = ld_parse_add(p);
  addr_t val2;
  while ((p->tok == '<' && p->tok_buf[1] == '<') || (p->tok == '>' && p->tok_buf[1] == '>'))
  {
    int op = p->tok;
    ld_next_token(p);
    val2 = ld_parse_add(p);
    if (op == '<')
      val <<= val2;
    else
      val >>= val2;
  }
  return val;
}

static addr_t ld_parse_and(LDParser *p)
{
  addr_t val = ld_parse_shift(p);
  while (p->tok == '&')
  {
    ld_next_token(p);
    val &= ld_parse_shift(p);
  }
  return val;
}

static addr_t ld_parse_xor(LDParser *p)
{
  addr_t val = ld_parse_and(p);
  while (p->tok == '^')
  {
    ld_next_token(p);
    val ^= ld_parse_and(p);
  }
  return val;
}

static addr_t ld_parse_or(LDParser *p)
{
  addr_t val = ld_parse_xor(p);
  while (p->tok == '|')
  {
    ld_next_token(p);
    val |= ld_parse_xor(p);
  }
  return val;
}

static addr_t ld_parse_expr(LDParser *p)
{
  p->expr_has_loadaddr = 0;
  p->expr_loadaddr_section_idx = -1;
  return ld_parse_or(p);
}

/* ================= Command Parsers ================= */

static int ld_parse_memory_attributes(LDParser *p)
{
  int attrs = 0;
  if (p->tok != '(')
    return 0;
  ld_next_token(p);
  while (p->tok == LDTOK_NAME && p->tok != ')')
  {
    for (const char *s = p->tok_buf; *s; s++)
    {
      switch (*s)
      {
      case 'r':
      case 'R':
        attrs |= LD_MEM_READ;
        break;
      case 'w':
      case 'W':
        attrs |= LD_MEM_WRITE;
        break;
      case 'x':
      case 'X':
        attrs |= LD_MEM_EXEC;
        break;
      case 'a':
      case 'A':
        attrs |= LD_MEM_ALLOC;
        break;
      case '!':
        break; /* invert - not fully supported */
      }
    }
    ld_next_token(p);
  }
  ld_expect(p, ')');
  return attrs;
}

static int ld_parse_memory(LDParser *p)
{
  TCCState *s1 = p->s1;
  LDMemoryRegion *mr;
  ld_next_token(p); /* skip 'MEMORY' */
  if (ld_expect(p, '{'))
    return -1;

  while (p->tok != '}' && p->tok != LDTOK_EOF)
  {
    if (p->tok == LDTOK_NAME)
    {
      if (p->ld->nb_memory_regions >= LD_MAX_MEMORY_REGIONS)
      {
        return tcc_error_noabort("too many memory regions");
      }
      mr = &p->ld->memory_regions[p->ld->nb_memory_regions];
      pstrcpy(mr->name, sizeof(mr->name), p->tok_buf);
      ld_next_token(p);

      /* Parse attributes (rwx) */
      mr->attributes = ld_parse_memory_attributes(p);

      /* Expect ':' */
      ld_expect(p, ':');

      /* Parse ORIGIN */
      if (p->tok == LDTOK_NAME &&
          (!strcmp(p->tok_buf, "ORIGIN") || !strcmp(p->tok_buf, "org") || !strcmp(p->tok_buf, "o")))
      {
        ld_next_token(p);
        ld_expect(p, '=');
        mr->origin = ld_parse_expr(p);
        printf("Memory region %s: ORIGIN=0x%llx\n", mr->name, (unsigned long long)mr->origin);
      }

      /* Expect ',' */
      if (p->tok == ',')
        ld_next_token(p);

      /* Parse LENGTH */
      if (p->tok == LDTOK_NAME &&
          (!strcmp(p->tok_buf, "LENGTH") || !strcmp(p->tok_buf, "len") || !strcmp(p->tok_buf, "l")))
      {
        ld_next_token(p);
        ld_expect(p, '=');
        mr->length = ld_parse_expr(p);
      }

      mr->current = mr->origin;
      p->ld->nb_memory_regions++;
    }
    else
    {
      ld_next_token(p);
    }
  }

  return ld_expect(p, '}');
}

static int ld_parse_phdrs(LDParser *p)
{
  TCCState *s1 = p->s1;
  LDPhdr *ph;
  ld_next_token(p); /* skip 'PHDRS' */
  if (ld_expect(p, '{'))
    return -1;

  while (p->tok != '}' && p->tok != LDTOK_EOF)
  {
    if (p->tok == LDTOK_NAME)
    {
      if (p->ld->nb_phdrs >= LD_MAX_PHDRS)
      {
        return tcc_error_noabort("too many program headers");
      }
      ph = &p->ld->phdrs[p->ld->nb_phdrs];
      pstrcpy(ph->name, sizeof(ph->name), p->tok_buf);
      ld_next_token(p);

      /* Parse type (PT_LOAD, PT_NULL, etc) */
      if (p->tok == LDTOK_NAME)
      {
        if (!strcmp(p->tok_buf, "PT_LOAD"))
          ph->type = PT_LOAD;
        else if (!strcmp(p->tok_buf, "PT_NULL"))
          ph->type = PT_NULL;
        else if (!strcmp(p->tok_buf, "PT_DYNAMIC"))
          ph->type = PT_DYNAMIC;
        else if (!strcmp(p->tok_buf, "PT_INTERP"))
          ph->type = PT_INTERP;
        else if (!strcmp(p->tok_buf, "PT_NOTE"))
          ph->type = PT_NOTE;
        else if (!strcmp(p->tok_buf, "PT_PHDR"))
          ph->type = PT_PHDR;
        else if (!strcmp(p->tok_buf, "PT_TLS"))
          ph->type = PT_TLS;
        else if (!strcmp(p->tok_buf, "PT_GNU_EH_FRAME"))
          ph->type = PT_GNU_EH_FRAME;
        else if (!strcmp(p->tok_buf, "PT_GNU_STACK"))
          ph->type = PT_GNU_STACK;
        else if (!strcmp(p->tok_buf, "PT_GNU_RELRO"))
          ph->type = PT_GNU_RELRO;
        ld_next_token(p);
      }

      /* Skip FLAGS and other options */
      while (p->tok != ';' && p->tok != '}' && p->tok != LDTOK_EOF)
        ld_next_token(p);

      if (p->tok == ';')
        ld_next_token(p);

      p->ld->nb_phdrs++;
    }
    else
    {
      ld_next_token(p);
    }
  }

  return ld_expect(p, '}');
}

static int ld_parse_section_pattern(LDParser *p, LDOutputSection *os, int keep)
{
  LDSectionPattern *pat;
  if (os->nb_patterns >= LD_MAX_SECTION_PATTERNS)
    return -1;

  pat = &os->patterns[os->nb_patterns];
  pat->keep = keep;
  pat->type = LD_PAT_GLOB;

  /* Parse file pattern (e.g., * or *.o) */
  if (p->tok == LDTOK_NAME || p->tok == '*')
  {
    /* Skip file pattern for now, just look for section pattern */
    ld_next_token(p);
  }

  /* Expect '(' for section pattern */
  if (p->tok == '(')
  {
    ld_next_token(p);
    /* Parse section patterns inside parentheses */
    while (p->tok != ')' && p->tok != LDTOK_EOF)
    {
      if (p->tok == LDTOK_NAME || p->tok == '.')
      {
        if (os->nb_patterns < LD_MAX_SECTION_PATTERNS)
        {
          pat = &os->patterns[os->nb_patterns];
          pstrcpy(pat->pattern, sizeof(pat->pattern), p->tok_buf);
          pat->keep = keep;
          pat->type = (strchr(pat->pattern, '*') != NULL) ? LD_PAT_GLOB : LD_PAT_EXACT;
          os->nb_patterns++;
        }
        ld_next_token(p);
      }
      else
      {
        ld_next_token(p);
      }
    }
    ld_expect(p, ')');
  }

  return 0;
}

static int ld_parse_output_section_contents(LDParser *p, LDOutputSection *os)
{
  /* Save the location counter at section entry to compute relative offsets */
  os->start_lc = p->ld->location_counter;

  while (p->tok != '}' && p->tok != LDTOK_EOF)
  {
    if (p->tok == '.')
    {
      /* Location counter assignment: . = expr */
      ld_next_token(p);
      if (p->tok == '=')
      {
        ld_next_token(p);
        p->ld->location_counter = ld_parse_expr(p);
        /* Track relative offset from section start */
        os->current_offset = p->ld->location_counter - os->start_lc;
        if (p->tok == ';')
          ld_next_token(p);
      }
    }
    else if (p->tok == LDTOK_NAME)
    {
      if (!strcmp(p->tok_buf, "KEEP"))
      {
        ld_next_token(p);
        ld_expect(p, '(');
        ld_parse_section_pattern(p, os, 1);
        ld_expect(p, ')');
      }
      else if (!strcmp(p->tok_buf, "PROVIDE") || !strcmp(p->tok_buf, "PROVIDE_HIDDEN"))
      {
        int hidden = !strcmp(p->tok_buf, "PROVIDE_HIDDEN");
        ld_next_token(p);
        ld_expect(p, '(');
        if (p->tok == LDTOK_NAME)
        {
          int idx = ld_script_find_or_create_symbol(p->ld, p->tok_buf);
          if (idx >= 0)
          {
            p->ld->symbols[idx].visibility = hidden ? LD_SYM_PROVIDE_HIDDEN : LD_SYM_PROVIDE;
            ld_next_token(p);
            if (p->tok == '=')
            {
              ld_next_token(p);
              addr_t val = ld_parse_expr(p);
              p->ld->symbols[idx].value = val;
              /* Compute offset from the evaluated value, not stale
               * current_offset */
              p->ld->symbols[idx].section_offset = val - os->start_lc;
              if (p->expr_has_loadaddr)
              {
                p->ld->symbols[idx].has_loadaddr = 1;
                p->ld->symbols[idx].loadaddr_section_idx = p->expr_loadaddr_section_idx;
                p->ld->symbols[idx].section_offset = val;
              }
              p->ld->symbols[idx].defined = 1;
              p->ld->symbols[idx].section_idx = p->ld->current_section_idx;
            }
          }
        }
        ld_expect(p, ')');
        if (p->tok == ';')
          ld_next_token(p);
      }
      else if (strchr(p->tok_buf, '*') || p->tok_buf[0] == '*')
      {
        /* Section pattern like *(.text*) */
        ld_parse_section_pattern(p, os, 0);
      }
      else
      {
        /* Could be symbol assignment: sym = expr */
        char name[128];
        pstrcpy(name, sizeof(name), p->tok_buf);
        ld_next_token(p);
        if (p->tok == '=')
        {
          int idx = ld_script_find_or_create_symbol(p->ld, name);
          if (idx >= 0)
          {
            ld_next_token(p);
            addr_t val = ld_parse_expr(p);
            p->ld->symbols[idx].value = val;
            /* Compute offset from the evaluated value, not stale current_offset
             */
            p->ld->symbols[idx].section_offset = val - os->start_lc;
            if (p->expr_has_loadaddr)
            {
              p->ld->symbols[idx].has_loadaddr = 1;
              p->ld->symbols[idx].loadaddr_section_idx = p->expr_loadaddr_section_idx;
              p->ld->symbols[idx].section_offset = val;
            }
            p->ld->symbols[idx].defined = 1;
            p->ld->symbols[idx].section_idx = p->ld->current_section_idx;
            if (p->tok == ';')
              ld_next_token(p);
          }
        }
        else
        {
          /* Regular section reference */
          /* Reparse as pattern */
        }
      }
    }
    else if (p->tok == '*')
    {
      ld_parse_section_pattern(p, os, 0);
    }
    else
    {
      ld_next_token(p);
    }
  }
  return 0;
}

static int ld_parse_sections(LDParser *p)
{
  TCCState *s1 = p->s1;
  LDOutputSection *os;
  ld_next_token(p); /* skip 'SECTIONS' */
  if (ld_expect(p, '{'))
    return -1;

  while (p->tok != '}' && p->tok != LDTOK_EOF)
  {
    if (p->tok == '.')
    {
      /* Could be output section or location counter */
      ld_next_token(p);

      if (p->tok == '=')
      {
        /* Location counter assignment at top level */
        ld_next_token(p);
        p->ld->location_counter = ld_parse_expr(p);
        if (p->tok == ';')
          ld_next_token(p);
        continue;
      }

      /* Output section definition starting with . */
      if (p->ld->nb_output_sections >= LD_MAX_OUTPUT_SECTIONS)
      {
        return tcc_error_noabort("too many output sections");
      }
      os = &p->ld->output_sections[p->ld->nb_output_sections];
      os->name[0] = '.';
      pstrcpy(os->name + 1, sizeof(os->name) - 1, p->tok_buf);
      os->memory_region_idx = -1;
      os->load_memory_region_idx = -1;
      os->phdr_idx = -1;
      os->align = 1;
      p->ld->current_section_idx = p->ld->nb_output_sections;
      ld_next_token(p);

      /* Optional address */
      if (p->tok == LDTOK_NUM)
      {
        os->address = p->tok_num;
        os->has_address = 1;
        ld_next_token(p);
      }

      /* Skip section type flags like (NOLOAD), (COPY), etc. */
      if (p->tok == '(')
      {
        while (p->tok != ')' && p->tok != LDTOK_EOF)
          ld_next_token(p);
        if (p->tok == ')')
          ld_next_token(p);
      }

      /* Section content in braces */
      if (p->tok == ':')
      {
        ld_next_token(p);
      }

      /* Track section start for relative offset calculation */
      os->current_offset = 0;
      os->start_lc = p->ld->location_counter;

      if (p->tok == '{')
      {
        ld_next_token(p);
        ld_parse_output_section_contents(p, os);
        ld_expect(p, '}');
      }

      /* Memory region: > region */
      if (p->tok == '>')
      {
        ld_next_token(p);
        if (p->tok == LDTOK_NAME)
        {
          os->memory_region_idx = ld_script_find_memory_region(p->ld, p->tok_buf);
          ld_next_token(p);
        }
      }

      /* Program header: :phdr */
      if (p->tok == ':')
      {
        ld_next_token(p);
        if (p->tok == LDTOK_NAME)
        {
          for (int i = 0; i < p->ld->nb_phdrs; i++)
          {
            if (!strcmp(p->ld->phdrs[i].name, p->tok_buf))
            {
              os->phdr_idx = i;
              break;
            }
          }
          ld_next_token(p);
        }
      }

      /* Load memory region: AT > region */
      if (p->tok == LDTOK_NAME && !strcmp(p->tok_buf, "AT"))
      {
        ld_next_token(p);
        if (p->tok == '>')
        {
          ld_next_token(p);
          if (p->tok == LDTOK_NAME)
          {
            os->load_memory_region_idx = ld_script_find_memory_region(p->ld, p->tok_buf);
            ld_next_token(p);
          }
        }
      }

      p->ld->nb_output_sections++;
    }
    else if (p->tok == LDTOK_NAME)
    {
      /* Could be symbol assignment or output section without leading dot */
      char name[128];
      pstrcpy(name, sizeof(name), p->tok_buf);
      ld_next_token(p);

      if (p->tok == '=')
      {
        /* Symbol assignment */
        int idx = ld_script_find_or_create_symbol(p->ld, name);
        if (idx >= 0)
        {
          ld_next_token(p);
          addr_t val = ld_parse_expr(p);
          p->ld->symbols[idx].value = val;
          if (p->expr_has_loadaddr)
          {
            p->ld->symbols[idx].has_loadaddr = 1;
            p->ld->symbols[idx].loadaddr_section_idx = p->expr_loadaddr_section_idx;
            p->ld->symbols[idx].section_offset = val;
          }
          p->ld->symbols[idx].defined = 1;
          if (p->tok == ';')
            ld_next_token(p);
        }
      }
      else if (p->tok == ':' || p->tok == '{' || p->tok == LDTOK_NUM || p->tok == '(')
      {
        /* Output section */
        if (p->ld->nb_output_sections >= LD_MAX_OUTPUT_SECTIONS)
        {
          return tcc_error_noabort("too many output sections");
        }
        os = &p->ld->output_sections[p->ld->nb_output_sections];
        pstrcpy(os->name, sizeof(os->name), name);
        os->memory_region_idx = -1;
        os->load_memory_region_idx = -1;
        os->phdr_idx = -1;
        os->current_offset = 0;
        p->ld->current_section_idx = p->ld->nb_output_sections;

        if (p->tok == LDTOK_NUM)
        {
          os->address = p->tok_num;
          os->has_address = 1;
          ld_next_token(p);
        }

        /* Skip section type flags like (NOLOAD), (COPY), etc. */
        if (p->tok == '(')
        {
          while (p->tok != ')' && p->tok != LDTOK_EOF)
            ld_next_token(p);
          if (p->tok == ')')
            ld_next_token(p);
        }

        if (p->tok == ':')
          ld_next_token(p);

        os->start_lc = p->ld->location_counter;

        if (p->tok == '{')
        {
          ld_next_token(p);
          ld_parse_output_section_contents(p, os);
          ld_expect(p, '}');
        }

        /* Memory region */
        if (p->tok == '>')
        {
          ld_next_token(p);
          if (p->tok == LDTOK_NAME)
          {
            os->memory_region_idx = ld_script_find_memory_region(p->ld, p->tok_buf);
            ld_next_token(p);
          }
        }

        /* Program header */
        if (p->tok == ':')
        {
          ld_next_token(p);
          if (p->tok == LDTOK_NAME)
          {
            for (int i = 0; i < p->ld->nb_phdrs; i++)
            {
              if (!strcmp(p->ld->phdrs[i].name, p->tok_buf))
              {
                os->phdr_idx = i;
                break;
              }
            }
            ld_next_token(p);
          }
        }

        /* Load memory region: AT > region */
        if (p->tok == LDTOK_NAME && !strcmp(p->tok_buf, "AT"))
        {
          ld_next_token(p);
          if (p->tok == '>')
          {
            ld_next_token(p);
            if (p->tok == LDTOK_NAME)
            {
              os->load_memory_region_idx = ld_script_find_memory_region(p->ld, p->tok_buf);
              ld_next_token(p);
            }
          }
        }

        p->ld->nb_output_sections++;
      }
    }
    else
    {
      ld_next_token(p);
    }
  }

  return ld_expect(p, '}');
}

static int ld_parse_entry(LDParser *p)
{
  TCCState *s1 = p->s1;
  ld_next_token(p); /* skip 'ENTRY' */
  if (ld_expect(p, '('))
    return -1;
  if (p->tok == LDTOK_NAME)
  {
    size_t len = strlen(p->tok_buf);
    if (len >= sizeof(p->ld->entry_point))
      return tcc_error_noabort("ENTRY name too long");
    memcpy(p->ld->entry_point, p->tok_buf, len + 1);
    p->ld->has_entry = 1;
    ld_next_token(p);
  }
  return ld_expect(p, ')');
}

/* ================= Public API ================= */

void ld_script_init(LDScript *ld)
{
  memset(ld, 0, sizeof(*ld));
  ld->current_section_idx = -1;
  ld->current_memory_region_idx = -1;
}

int ld_script_parse(TCCState *s1, LDScript *ld, int fd)
{
  LDParser parser;
  int ret = 0;

  memset(&parser, 0, sizeof(parser));
  parser.s1 = s1;
  parser.ld = ld;
  parser.fd = fd;
  parser.str = NULL;
  parser.cc = -1;

  ld_next_token(&parser);

  while (parser.tok != LDTOK_EOF && ret == 0)
  {
    if (parser.tok == LDTOK_NAME)
    {
      if (!strcmp(parser.tok_buf, "MEMORY"))
      {
        ret = ld_parse_memory(&parser);
      }
      else if (!strcmp(parser.tok_buf, "PHDRS"))
      {
        ret = ld_parse_phdrs(&parser);
      }
      else if (!strcmp(parser.tok_buf, "SECTIONS"))
      {
        ret = ld_parse_sections(&parser);
      }
      else if (!strcmp(parser.tok_buf, "ENTRY"))
      {
        ret = ld_parse_entry(&parser);
      }
      else if (!strcmp(parser.tok_buf, "OUTPUT_FORMAT") || !strcmp(parser.tok_buf, "OUTPUT_ARCH") ||
               !strcmp(parser.tok_buf, "TARGET") || !strcmp(parser.tok_buf, "SEARCH_DIR") ||
               !strcmp(parser.tok_buf, "INPUT") || !strcmp(parser.tok_buf, "GROUP") ||
               !strcmp(parser.tok_buf, "OUTPUT") || !strcmp(parser.tok_buf, "INCLUDE"))
      {
        /* Skip these commands for now */
        ld_next_token(&parser);
        if (parser.tok == '(')
        {
          int depth = 1;
          ld_next_token(&parser);
          while (depth > 0 && parser.tok != LDTOK_EOF)
          {
            if (parser.tok == '(')
              depth++;
            else if (parser.tok == ')')
              depth--;
            ld_next_token(&parser);
          }
        }
      }
      else
      {
        /* Unknown command - might be top-level symbol assignment */
        char name[128];
        pstrcpy(name, sizeof(name), parser.tok_buf);
        ld_next_token(&parser);
        if (parser.tok == '=')
        {
          int idx = ld_script_find_or_create_symbol(ld, name);
          if (idx >= 0)
          {
            ld_next_token(&parser);
            addr_t val = ld_parse_expr(&parser);
            ld->symbols[idx].value = val;
            if (parser.expr_has_loadaddr)
            {
              ld->symbols[idx].has_loadaddr = 1;
              ld->symbols[idx].loadaddr_section_idx = parser.expr_loadaddr_section_idx;
              ld->symbols[idx].section_offset = val;
            }
            ld->symbols[idx].defined = 1;
            if (parser.tok == ';')
              ld_next_token(&parser);
          }
        }
      }
    }
    else
    {
      ld_next_token(&parser);
    }
  }

  return ret;
}

int ld_script_parse_string(TCCState *s1, LDScript *ld, const char *script)
{
  LDParser parser;
  int ret = 0;

  memset(&parser, 0, sizeof(parser));
  parser.s1 = s1;
  parser.ld = ld;
  parser.fd = -1;
  parser.str = script;
  parser.str_pos = 0;
  parser.cc = -1;

  ld_next_token(&parser);

  while (parser.tok != LDTOK_EOF && ret == 0)
  {
    if (parser.tok == LDTOK_NAME)
    {
      if (!strcmp(parser.tok_buf, "MEMORY"))
      {
        ret = ld_parse_memory(&parser);
      }
      else if (!strcmp(parser.tok_buf, "PHDRS"))
      {
        ret = ld_parse_phdrs(&parser);
      }
      else if (!strcmp(parser.tok_buf, "SECTIONS"))
      {
        ret = ld_parse_sections(&parser);
      }
      else if (!strcmp(parser.tok_buf, "ENTRY"))
      {
        ret = ld_parse_entry(&parser);
      }
      else
      {
        ld_next_token(&parser);
      }
    }
    else
    {
      ld_next_token(&parser);
    }
  }

  return ret;
}

int ld_script_find_memory_region(LDScript *ld, const char *name)
{
  for (int i = 0; i < ld->nb_memory_regions; i++)
  {
    if (!strcmp(ld->memory_regions[i].name, name))
      return i;
  }
  return -1;
}

int ld_script_find_output_section(LDScript *ld, const char *name)
{
  for (int i = 0; i < ld->nb_output_sections; i++)
  {
    if (!strcmp(ld->output_sections[i].name, name))
      return i;
  }
  return -1;
}

int ld_script_find_or_create_symbol(LDScript *ld, const char *name)
{
  int i, idx;
  /* First, try to find existing symbol */
  for (i = 0; i < ld->nb_symbols; i++)
  {
    if (!strcmp(ld->symbols[i].name, name))
      return i;
  }
  /* Create new symbol */
  if (ld->nb_symbols >= LD_MAX_SYMBOLS)
    return -1;
  idx = ld->nb_symbols++;
  pstrcpy(ld->symbols[idx].name, sizeof(ld->symbols[idx].name), name);
  ld->symbols[idx].value = 0;
  ld->symbols[idx].defined = 0;
  ld->symbols[idx].visibility = LD_SYM_GLOBAL;
  ld->symbols[idx].section_idx = -1;
  ld->symbols[idx].has_loadaddr = 0;
  ld->symbols[idx].loadaddr_section_idx = -1;
  return idx;
}

/* Check if a section should be kept (not garbage collected) based on linker
 * script KEEP directives */
int ld_section_should_keep(LDScript *ld, const char *section_name)
{
  if (!ld)
    return 0;
  for (int i = 0; i < ld->nb_output_sections; i++)
  {
    LDOutputSection *os = &ld->output_sections[i];
    for (int j = 0; j < os->nb_patterns; j++)
    {
      LDSectionPattern *pat = &os->patterns[j];
      if (pat->keep && ld_section_matches_pattern(section_name, pat->pattern))
      {
        return 1;
      }
    }
  }
  return 0;
}

/* Simple glob matching */
int ld_section_matches_pattern(const char *section_name, const char *pattern)
{
  const char *s = section_name;
  const char *p = pattern;

  while (*p && *s)
  {
    if (*p == '*')
    {
      p++;
      if (*p == '\0')
        return 1; /* trailing * matches everything */
      /* Match as many characters as possible */
      while (*s)
      {
        if (ld_section_matches_pattern(s, p))
          return 1;
        s++;
      }
      return 0;
    }
    else if (*p == '?')
    {
      p++;
      s++;
    }
    else if (*p == *s)
    {
      p++;
      s++;
    }
    else
    {
      return 0;
    }
  }

  /* Skip trailing wildcards in pattern */
  while (*p == '*')
    p++;

  return (*p == '\0' && *s == '\0');
}

void ld_script_dump(LDScript *ld)
{
  printf("=== Linker Script Dump ===\n");

  if (ld->has_entry)
    printf("ENTRY(%s)\n", ld->entry_point);

  printf("\nMEMORY {\n");
  for (int i = 0; i < ld->nb_memory_regions; i++)
  {
    LDMemoryRegion *mr = &ld->memory_regions[i];
    printf("  %s (%c%c%c) : ORIGIN = 0x%lx, LENGTH = 0x%lx\n", mr->name, (mr->attributes & LD_MEM_READ) ? 'r' : '-',
           (mr->attributes & LD_MEM_WRITE) ? 'w' : '-', (mr->attributes & LD_MEM_EXEC) ? 'x' : '-',
           (unsigned long)mr->origin, (unsigned long)mr->length);
  }
  printf("}\n");

  printf("\nPHDRS {\n");
  for (int i = 0; i < ld->nb_phdrs; i++)
  {
    LDPhdr *ph = &ld->phdrs[i];
    printf("  %s PT_type=%d\n", ph->name, ph->type);
  }
  printf("}\n");

  printf("\nSECTIONS {\n");
  for (int i = 0; i < ld->nb_output_sections; i++)
  {
    LDOutputSection *os = &ld->output_sections[i];
    printf("  %s", os->name);
    if (os->has_address)
      printf(" 0x%lx", (unsigned long)os->address);
    printf(" : {\n");
    for (int j = 0; j < os->nb_patterns; j++)
    {
      printf("    %s%s%s\n", os->patterns[j].keep ? "KEEP(" : "", os->patterns[j].pattern,
             os->patterns[j].keep ? ")" : "");
    }
    printf("  }");
    if (os->memory_region_idx >= 0)
      printf(" > %s", ld->memory_regions[os->memory_region_idx].name);
    if (os->load_memory_region_idx >= 0)
      printf(" AT > %s", ld->memory_regions[os->load_memory_region_idx].name);
    if (os->phdr_idx >= 0)
      printf(" :%s", ld->phdrs[os->phdr_idx].name);
    printf("\n");
  }
  printf("}\n");

  printf("\nSymbols:\n");
  for (int i = 0; i < ld->nb_symbols; i++)
  {
    LDSymbol *sym = &ld->symbols[i];
    printf("  %s = 0x%lx (%s)\n", sym->name, (unsigned long)sym->value, sym->defined ? "defined" : "undefined");
  }

  printf("=== End Dump ===\n");
}

/* Add standard linker symbols */
int ld_script_add_standard_symbols(TCCState *s1, LDScript *ld)
{
  /* These symbols will be resolved during layout_sections */
  static const char *standard_syms[] = {"__bss_start__",
                                        "__bss_start",
                                        "__bss_end__",
                                        "_bss_end__",
                                        "__data_start__",
                                        "_edata",
                                        "__data_end__",
                                        "__end__",
                                        "_end",
                                        "end",
                                        "__text_start__",
                                        "_stext",
                                        "__text_end__",
                                        "_etext",
                                        "__heap_start__",
                                        "__heap_end__",
                                        "__stack_start__",
                                        "__stack_end__",
                                        "__rodata_start__",
                                        "__rodata_end__",
                                        NULL};

  for (int i = 0; standard_syms[i]; i++)
  {
    ld_script_find_or_create_symbol(ld, standard_syms[i]);
  }

  return 0;
}
