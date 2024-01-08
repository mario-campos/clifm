/* mem.h - Some memory wrapper functions */

/*
 * This file is part of CliFM
 *
 * Copyright (C) 2016-2024, L. Abramovich <leo.clifm@outlook.com>
 * All rights reserved.

 * CliFM is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * CliFM is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
*/

/* This file is included by aux.h */

#ifndef CLIFM_MEM_H
#define CLIFM_MEM_H

__BEGIN_DECLS

void *xnrealloc(void *ptr, const size_t nmemb, const size_t size);
void *xcalloc(const size_t nmemb, const size_t size);
void *xnmalloc(const size_t nmemb, const size_t size);

__END_DECLS

#endif /* CLIFM_MEM_H */
