/* Supporting functions for resolving DATA statement.
   Copyright (C) 2002, 2003, 2004, 2005, 2006, 2007, 2008, 2009, 2010
   Free Software Foundation, Inc.
   Contributed by Lifang Zeng <zlf605@hotmail.com>

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 3, or (at your option) any later
version.

GCC is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */


/* Notes for DATA statement implementation:
									       
   We first assign initial value to each symbol by gfc_assign_data_value
   during resolving DATA statement. Refer to check_data_variable and
   traverse_data_list in resolve.c.
									       
   The complexity exists in the handling of array section, implied do
   and array of struct appeared in DATA statement.
									       
   We call gfc_conv_structure, gfc_con_array_array_initializer,
   etc., to convert the initial value. Refer to trans-expr.c and
   trans-array.c.  */

#include "config.h"
#include "system.h"
#include "gfortran.h"
#include "data.h"
#include "constructor.h"

static void formalize_init_expr (gfc_expr *);

/* Calculate the array element offset.  */

static void
get_array_index (gfc_array_ref *ar, mpz_t *offset)
{
  gfc_expr *e;
  int i;
  mpz_t delta;
  mpz_t tmp;

  mpz_init (tmp);
  mpz_set_si (*offset, 0);
  mpz_init_set_si (delta, 1);
  for (i = 0; i < ar->dimen; i++)
    {
      e = gfc_copy_expr (ar->start[i]);
      gfc_simplify_expr (e, 1);

      if ((gfc_is_constant_expr (ar->as->lower[i]) == 0)
	  || (gfc_is_constant_expr (ar->as->upper[i]) == 0)
	  || (gfc_is_constant_expr (e) == 0))
	gfc_error ("non-constant array in DATA statement %L", &ar->where);

      mpz_set (tmp, e->value.integer);
      mpz_sub (tmp, tmp, ar->as->lower[i]->value.integer);
      mpz_mul (tmp, tmp, delta);
      mpz_add (*offset, tmp, *offset);

      mpz_sub (tmp, ar->as->upper[i]->value.integer,
	       ar->as->lower[i]->value.integer);
      mpz_add_ui (tmp, tmp, 1);
      mpz_mul (delta, tmp, delta);
    }
  mpz_clear (delta);
  mpz_clear (tmp);
}

/* Find if there is a constructor which component is equal to COM.
   TODO: remove this, use symbol.c(gfc_find_component) instead.  */

static gfc_constructor *
find_con_by_component (gfc_component *com, gfc_constructor_base base)
{
  gfc_constructor *c;

  for (c = gfc_constructor_first (base); c; c = gfc_constructor_next (c))
    if (com == c->n.component)
      return c;

  return NULL;
}


/* Create a character type initialization expression from RVALUE.
   TS [and REF] describe [the substring of] the variable being initialized.
   INIT is the existing initializer, not NULL.  Initialization is performed
   according to normal assignment rules.  */

static gfc_expr *
create_character_intializer (gfc_expr *init, gfc_typespec *ts,
			     gfc_ref *ref, gfc_expr *rvalue)
{
  int len, start, end;
  gfc_char_t *dest;
	    
  gfc_extract_int (ts->u.cl->length, &len);

  if (init == NULL)
    {
      /* Create a new initializer.  */
      init = gfc_get_character_expr (ts->kind, NULL, NULL, len);
      init->ts = *ts;
    }

  dest = init->value.character.string;

  if (ref)
    {
      gfc_expr *start_expr, *end_expr;

      gcc_assert (ref->type == REF_SUBSTRING);

      /* Only set a substring of the destination.  Fortran substring bounds
	 are one-based [start, end], we want zero based [start, end).  */
      start_expr = gfc_copy_expr (ref->u.ss.start);
      end_expr = gfc_copy_expr (ref->u.ss.end);

      if ((gfc_simplify_expr (start_expr, 1) == FAILURE)
	  || (gfc_simplify_expr (end_expr, 1)) == FAILURE)
	{
	  gfc_error ("failure to simplify substring reference in DATA "
		     "statement at %L", &ref->u.ss.start->where);
	  return NULL;
	}

      gfc_extract_int (start_expr, &start);
      start--;
      gfc_extract_int (end_expr, &end);
    }
  else
    {
      /* Set the whole string.  */
      start = 0;
      end = len;
    }

  /* Copy the initial value.  */
  if (rvalue->ts.type == BT_HOLLERITH)
    len = rvalue->representation.length;
  else
    len = rvalue->value.character.length;

  if (len > end - start)
    {
      gfc_warning_now ("Initialization string starting at %L was "
		       "truncated to fit the variable (%d/%d)",
		       &rvalue->where, end - start, len);
      len = end - start;
    }

  if (rvalue->ts.type == BT_HOLLERITH)
    {
      int i;
      for (i = 0; i < len; i++)
	dest[start+i] = rvalue->representation.string[i];
    }
  else
    memcpy (&dest[start], rvalue->value.character.string,
	    len * sizeof (gfc_char_t));

  /* Pad with spaces.  Substrings will already be blanked.  */
  if (len < end - start && ref == NULL)
    gfc_wide_memset (&dest[start + len], ' ', end - (start + len));

  if (rvalue->ts.type == BT_HOLLERITH)
    {
      init->representation.length = init->value.character.length;
      init->representation.string
	= gfc_widechar_to_char (init->value.character.string,
				init->value.character.length);
    }

  return init;
}


/* Assign the initial value RVALUE to  LVALUE's symbol->value. If the
   LVALUE already has an initialization, we extend this, otherwise we
   create a new one.  */

gfc_try
gfc_assign_data_value (gfc_expr *lvalue, gfc_expr *rvalue, mpz_t index)
{
  gfc_ref *ref;
  gfc_expr *init;
  gfc_expr *expr;
  gfc_constructor *con;
  gfc_constructor *last_con;
  gfc_symbol *symbol;
  gfc_typespec *last_ts;
  mpz_t offset;

  symbol = lvalue->symtree->n.sym;
  init = symbol->value;
  last_ts = &symbol->ts;
  last_con = NULL;
  mpz_init_set_si (offset, 0);

  /* Find/create the parent expressions for subobject references.  */
  for (ref = lvalue->ref; ref; ref = ref->next)
    {
      /* Break out of the loop if we find a substring.  */
      if (ref->type == REF_SUBSTRING)
	{
	  /* A substring should always be the last subobject reference.  */
	  gcc_assert (ref->next == NULL);
	  break;
	}

      /* Use the existing initializer expression if it exists.  Otherwise
	 create a new one.  */
      if (init == NULL)
	expr = gfc_get_expr ();
      else
	expr = init;

      /* Find or create this element.  */
      switch (ref->type)
	{
	case REF_ARRAY:
	  if (ref->u.ar.as->rank == 0)
	    {
	      gcc_assert (ref->u.ar.as->corank > 0);
	      if (init == NULL)
		gfc_free (expr);
	      continue;
	    }

	  if (init && expr->expr_type != EXPR_ARRAY)
	    {
	      gfc_error ("'%s' at %L already is initialized at %L",
			 lvalue->symtree->n.sym->name, &lvalue->where,
			 &init->where);
	      goto abort;
	    }

	  if (init == NULL)
	    {
	      /* The element typespec will be the same as the array
		 typespec.  */
	      expr->ts = *last_ts;
	      /* Setup the expression to hold the constructor.  */
	      expr->expr_type = EXPR_ARRAY;
	      expr->rank = ref->u.ar.as->rank;
	    }

	  if (ref->u.ar.type == AR_ELEMENT)
	    get_array_index (&ref->u.ar, &offset);
	  else
	    mpz_set (offset, index);

	  /* Check the bounds.  */
	  if (mpz_cmp_si (offset, 0) < 0)
	    {
	      gfc_error ("Data element below array lower bound at %L",
			 &lvalue->where);
	      goto abort;
	    }
	  else
	    {
	      mpz_t size;
	      if (spec_size (ref->u.ar.as, &size) == SUCCESS)
		{
		  if (mpz_cmp (offset, size) >= 0)
		    {
		      mpz_clear (size);
		      gfc_error ("Data element above array upper bound at %L",
		                 &lvalue->where);
		      goto abort;
		    }
		  mpz_clear (size);
		}
	    }

	  con = gfc_constructor_lookup (expr->value.constructor,
					mpz_get_si (offset));
	  if (!con)
	    {
	      con = gfc_constructor_insert_expr (&expr->value.constructor,
						 NULL, &rvalue->where,
						 mpz_get_si (offset));
	    }
	  break;

	case REF_COMPONENT:
	  if (init == NULL)
	    {
	      /* Setup the expression to hold the constructor.  */
	      expr->expr_type = EXPR_STRUCTURE;
	      expr->ts.type = BT_DERIVED;
	      expr->ts.u.derived = ref->u.c.sym;
	    }
	  else
	    gcc_assert (expr->expr_type == EXPR_STRUCTURE);
	  last_ts = &ref->u.c.component->ts;

	  /* Find the same element in the existing constructor.  */
	  con = find_con_by_component (ref->u.c.component,
				       expr->value.constructor);

	  if (con == NULL)
	    {
	      /* Create a new constructor.  */
	      con = gfc_constructor_append_expr (&expr->value.constructor,
						 NULL, NULL);
	      con->n.component = ref->u.c.component;
	    }
	  break;

	default:
	  gcc_unreachable ();
	}

      if (init == NULL)
	{
	  /* Point the container at the new expression.  */
	  if (last_con == NULL)
	    symbol->value = expr;
	  else
	    last_con->expr = expr;
	}
      init = con->expr;
      last_con = con;
    }

  mpz_clear (offset);

  if (ref || last_ts->type == BT_CHARACTER)
    {
      if (lvalue->ts.u.cl->length == NULL && !(ref && ref->u.ss.length != NULL))
	return FAILURE;
      expr = create_character_intializer (init, last_ts, ref, rvalue);
    }
  else
    {
      /* Overwriting an existing initializer is non-standard but usually only
	 provokes a warning from other compilers.  */
      if (init != NULL)
	{
	  /* Order in which the expressions arrive here depends on whether
	     they are from data statements or F95 style declarations.
	     Therefore, check which is the most recent.  */
	  expr = (LOCATION_LINE (init->where.lb->location)
		  > LOCATION_LINE (rvalue->where.lb->location))
	       ? init : rvalue;
	  if (gfc_notify_std (GFC_STD_GNU,"Extension: "
			      "re-initialization of '%s' at %L",
			      symbol->name, &expr->where) == FAILURE)
	    return FAILURE;
	}

      expr = gfc_copy_expr (rvalue);
      if (!gfc_compare_types (&lvalue->ts, &expr->ts))
	gfc_convert_type (expr, &lvalue->ts, 0);
    }

  if (last_con == NULL)
    symbol->value = expr;
  else
    last_con->expr = expr;

  return SUCCESS;

abort:
  mpz_clear (offset);
  return FAILURE;
}


/* Similarly, but initialize REPEAT consecutive values in LVALUE the same
   value in RVALUE.  */

gfc_try
gfc_assign_data_value_range (gfc_expr *lvalue, gfc_expr *rvalue,
			     mpz_t index, mpz_t repeat)
{
  mpz_t offset, last_offset;
  gfc_try t;

  mpz_init (offset);
  mpz_init (last_offset);
  mpz_add (last_offset, index, repeat);

  t = SUCCESS;
  for (mpz_set(offset, index) ; mpz_cmp(offset, last_offset) < 0;
		   mpz_add_ui (offset, offset, 1))
    if (gfc_assign_data_value (lvalue, rvalue, offset) == FAILURE)
      {
	t = FAILURE;
	break;
      }

  mpz_clear (offset);
  mpz_clear (last_offset);

  return t;
}


/* Modify the index of array section and re-calculate the array offset.  */

void 
gfc_advance_section (mpz_t *section_index, gfc_array_ref *ar,
		     mpz_t *offset_ret)
{
  int i;
  mpz_t delta;
  mpz_t tmp; 
  bool forwards;
  int cmp;

  for (i = 0; i < ar->dimen; i++)
    {
      if (ar->dimen_type[i] != DIMEN_RANGE)
	continue;

      if (ar->stride[i])
	{
	  mpz_add (section_index[i], section_index[i],
		   ar->stride[i]->value.integer);
	if (mpz_cmp_si (ar->stride[i]->value.integer, 0) >= 0)
	  forwards = true;
	else
	  forwards = false;
	}
      else
	{
	  mpz_add_ui (section_index[i], section_index[i], 1);
	  forwards = true;
	}
      
      if (ar->end[i])
	cmp = mpz_cmp (section_index[i], ar->end[i]->value.integer);
      else
	cmp = mpz_cmp (section_index[i], ar->as->upper[i]->value.integer);

      if ((cmp > 0 && forwards) || (cmp < 0 && !forwards))
	{
	  /* Reset index to start, then loop to advance the next index.  */
	  if (ar->start[i])
	    mpz_set (section_index[i], ar->start[i]->value.integer);
	  else
	    mpz_set (section_index[i], ar->as->lower[i]->value.integer);
	}
      else
	break;
    }

  mpz_set_si (*offset_ret, 0);
  mpz_init_set_si (delta, 1);
  mpz_init (tmp);
  for (i = 0; i < ar->dimen; i++)
    {
      mpz_sub (tmp, section_index[i], ar->as->lower[i]->value.integer);
      mpz_mul (tmp, tmp, delta);
      mpz_add (*offset_ret, tmp, *offset_ret);

      mpz_sub (tmp, ar->as->upper[i]->value.integer, 
	       ar->as->lower[i]->value.integer);
      mpz_add_ui (tmp, tmp, 1);
      mpz_mul (delta, tmp, delta);
    }
  mpz_clear (tmp);
  mpz_clear (delta);
}


/* Rearrange a structure constructor so the elements are in the specified
   order.  Also insert NULL entries if necessary.  */

static void
formalize_structure_cons (gfc_expr *expr)
{
  gfc_constructor_base base = NULL;
  gfc_constructor *cur;
  gfc_component *order;

  /* Constructor is already formalized.  */
  cur = gfc_constructor_first (expr->value.constructor);
  if (!cur || cur->n.component == NULL)
    return;

  for (order = expr->ts.u.derived->components; order; order = order->next)
    {
      cur = find_con_by_component (order, expr->value.constructor);
      if (cur)
	gfc_constructor_append_expr (&base, cur->expr, &cur->expr->where);
      else
	gfc_constructor_append_expr (&base, NULL, NULL);
    }

  /* For all what it's worth, one would expect
       gfc_constructor_free (expr->value.constructor);
     here. However, if the constructor is actually free'd,
     hell breaks loose in the testsuite?!  */

  expr->value.constructor = base;
}


/* Make sure an initialization expression is in normalized form, i.e., all
   elements of the constructors are in the correct order.  */

static void
formalize_init_expr (gfc_expr *expr)
{
  expr_t type;
  gfc_constructor *c;

  if (expr == NULL)
    return;

  type = expr->expr_type;
  switch (type)
    {
    case EXPR_ARRAY:
      for (c = gfc_constructor_first (expr->value.constructor);
	   c; c = gfc_constructor_next (c))
	formalize_init_expr (c->expr);

    break;

    case EXPR_STRUCTURE:
      formalize_structure_cons (expr);
      break;

    default:
      break;
    }
}


/* Resolve symbol's initial value after all data statement.  */

void
gfc_formalize_init_value (gfc_symbol *sym)
{
  formalize_init_expr (sym->value);
}


/* Get the integer value into RET_AS and SECTION from AS and AR, and return
   offset.  */
 
void
gfc_get_section_index (gfc_array_ref *ar, mpz_t *section_index, mpz_t *offset)
{
  int i;
  mpz_t delta;
  mpz_t tmp;

  mpz_set_si (*offset, 0);
  mpz_init (tmp);
  mpz_init_set_si (delta, 1);
  for (i = 0; i < ar->dimen; i++)
    {
      mpz_init (section_index[i]);
      switch (ar->dimen_type[i])
	{
	case DIMEN_ELEMENT:
	case DIMEN_RANGE:
	  if (ar->start[i])
	    {
	      mpz_sub (tmp, ar->start[i]->value.integer,
		       ar->as->lower[i]->value.integer);
	      mpz_mul (tmp, tmp, delta);
	      mpz_add (*offset, tmp, *offset);
	      mpz_set (section_index[i], ar->start[i]->value.integer);
	    }
	  else
	      mpz_set (section_index[i], ar->as->lower[i]->value.integer);
	  break;

	case DIMEN_VECTOR:
	  gfc_internal_error ("TODO: Vector sections in data statements");

	default:
	  gcc_unreachable ();
	}

      mpz_sub (tmp, ar->as->upper[i]->value.integer, 
	       ar->as->lower[i]->value.integer);
      mpz_add_ui (tmp, tmp, 1);
      mpz_mul (delta, tmp, delta);
    }

  mpz_clear (tmp);
  mpz_clear (delta);
}

