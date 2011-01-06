/* ast.h

   AST representation of prediction programs.

   Copyright (c) 2010 Matthias Kramm <kramm@quiss.org>
 
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#ifndef __ast_h__
#define __ast_h__

#include <stdio.h>
#include <stdbool.h>
#include "model.h"
#include "constant.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _node node_t;
typedef struct _nodetype nodetype_t;
typedef struct _environment environment_t;

#define NODE_FLAG_HAS_CHILDREN 1
#define NODE_FLAG_HAS_VALUE 2

struct _environment {
    row_t* row;
};

struct _nodetype {
    char*name;
    int flags;
    int min_args;
    int max_args;
    uint8_t opcode;
    constant_t (*eval)(node_t*n, environment_t* params);
};

struct _node {
    nodetype_t*type;
    node_t*parent;
    union {
        struct {
            node_t**child;
            int num_children;
        };
	constant_t value;
    };
};

/* all known node types & opcodes */
#define LIST_NODES \
    NODE(0x71, node_root) \
    NODE(0x01, node_if) \
    NODE(0x02, node_add) \
    NODE(0x03, node_lt) \
    NODE(0x04, node_lte) \
    NODE(0x05, node_gt) \
    NODE(0x06, node_in) \
    NODE(0x07, node_not) \
    NODE(0x08, node_var) \
    NODE(0x09, node_category) \
    NODE(0x0a, node_array) \
    NODE(0x0b, node_float) \
    NODE(0x0c, node_string)

#define NODE(opcode, name) extern nodetype_t name;
LIST_NODES
#undef NODE

extern nodetype_t* nodelist[];
void nodelist_init();
uint8_t node_get_opcode(node_t*n);

node_t* node_new(nodetype_t*t, ...);
void node_free(node_t*n);
constant_t node_eval(node_t*n,environment_t* e);
void node_print(node_t*n);


/* the following convenience macros allow to write code
   in the following matter:

   START_CODE
    IF 
      GT
	ADD
	  VAR(1)
	  VAR(1)
        VAR(3)
    THEN
      RETURN(1)
    ELSE
      RETURN(1)
   END_CODE

*/
#define NODE_BEGIN(new_type,args...) \
	    do { \
		node_t* new_node = node_new(new_type,##args); \
		if(current_node) { \
		    assert(current_node->type); \
		    assert(current_node->type->name); \
		    if(current_node->num_children >= current_node->type->max_args) { \
			fprintf(stderr, "Too many arguments (%d) to node (max %d args)\n", \
				current_node->num_children, \
				current_node->type->max_args); \
		    } \
		    assert(current_node->num_children < current_node->type->max_args); \
		    current_node->child[current_node->num_children++] = new_node; \
		} \
		if((new_node->type->flags) & NODE_FLAG_HAS_CHILDREN) { \
		    new_node->parent = current_node; \
		    current_node = new_node; \
		} \
	    } while(0);

#define NODE_CLOSE do {assert(current_node->num_children >= current_node->type->min_args && \
                           current_node->num_children <= current_node->type->max_args);\
                    current_node = current_node->parent; \
                   } while(0)

#define START_CODE(program) \
	node_t* program; \
	{ \
	    node_t*current_node = program = node_new(&node_root); \
	    program = current_node;

#define END_CODE \
	}

#define END NODE_CLOSE
#define IF NODE_BEGIN(&node_if)
#define NOT NODE_BEGIN(&node_not)
#define THEN assert(current_node && current_node->type == &node_if && current_node->num_children == 1);
#define ELSE assert(current_node && current_node->type == &node_if && current_node->num_children == 2);
#define ADD NODE_BEGIN(&node_add)
#define LT NODE_BEGIN(&node_lt)
#define LTE NODE_BEGIN(&node_lte)
#define GT NODE_BEGIN(&node_gt)
#define IN NODE_BEGIN(&node_in)
#define VAR(i) NODE_BEGIN(&node_var, i)
#define RETURN(n) do {VERIFY_INT(n);NODE_BEGIN(&node_category, n)}while(0);
#define RETURN_STRING(s) do {VERIFY_STRING(s);NODE_BEGIN(&node_string, s)}while(0);
#define FLOAT_CONSTANT(f) NODE_BEGIN(&node_float, f)
#define STRING_CONSTANT(s) NODE_BEGIN(&node_string, s)
#define ARRAY_CONSTANT(args...) NODE_BEGIN(&node_array, ##args)

#define VERIFY_INT(n) do{if(0)(((char*)0)[(n)]);}while(0)
#define VERIFY_STRING(s) do{if(0){(s)[0];};}while(0)

#ifdef __cplusplus
}
#endif


#endif
