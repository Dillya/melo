/*
 * melo_jsonrpc.c: JSON-RPC 2.0 Parser
 *
 * Copyright (C) 2016 Alexandre Dilly <dillya@sparod.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include <string.h>

#include "melo_jsonrpc.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/**
 * SECTION:melo_jsonrpc
 * @title: MeloJsonRPC
 * @short_description: A JSON-RPC 2.0 parser
 *
 * Melo implements a global JSON-RPC parser which handles many JSON-RPC methods
 * registered with melo_jsonrpc_register_method() or with
 * melo_jsonrpc_register_methods().
 *
 * Any code can then call the melo_jsonrpc_parse_request() to parse a string
 * containing a JSON-RPC request.
 *
 * It allows possibility to any object in Melo to register its own JSON-RPC
 * methods and be called when a new request is received through an HTTP server
 * or any else type of transport protocol like WebSocket.
 *
 * The melo_jsonrpc_parse_request() should be only called by the main
 * application and not from any Melo object like #MeloModule.
 */

typedef struct _MeloJSONRPCInternalMethod {
  /* Schema nodes */
  JsonArray *params;
  JsonObject *result;
  /* Callback */
  MeloJSONRPCCallback callback;
  gpointer user_data;

} MeloJSONRPCInternalMethod;

/* List of groups and methods */
G_LOCK_DEFINE_STATIC (melo_jsonrpc_mutex);
static GHashTable *melo_jsonrpc_methods = NULL;

/* Helpers */
static gchar *melo_jsonrpc_node_to_string (JsonNode *node);
static JsonNode *melo_jsonrpc_build_error (const char *id, gint64 nid,
                                           MeloJSONRPCError error_code,
                                           const char *error_format, ...);
static gchar *melo_jsonrpc_build_error_str (MeloJSONRPCError error_code,
                                            const char *error_format, ...);
static JsonNode *melo_jsonrpc_build_response_node (JsonNode *result,
                                                   JsonNode *error,
                                                   const gchar *id,
                                                   gint64 nid);

/* Register a JSON-RPC method */
static void
melo_jsonrpc_free_method (gpointer data)
{
  MeloJSONRPCInternalMethod *m = data;

  /* Free nodes */
  if (m->params)
    json_array_unref (m->params);
  if (m->result)
    json_object_unref (m->result);

  /* Free method */
  g_slice_free (MeloJSONRPCInternalMethod, m);
}

/**
 * melo_jsonrpc_register_method:
 * @group: prefix of the method
 * @method: the method name
 * @params: the schema of the parameters accepted as JSON
 * @result: the schema of the result provided as JSON
 * @callback: the callback of type #MeloJSONRPCCallback to call when @method,
 *    @params and @result are matching
 * @user_data: the user data to use when calling @callback
 *
 * Register one JSON-RPC method named @method and prefixed with @group.
 * The final method will be as "@group.@name".
 * For more details on the @params and @method, please see #MeloJSONRPCMethod.
 *
 * Returns: %TRUE if method has been registered, %FALSE otherwise.
 */
gboolean
melo_jsonrpc_register_method (const gchar *group, const gchar *method,
                              JsonArray *params, JsonObject *result,
                              MeloJSONRPCCallback callback,
                              gpointer user_data)
{
  MeloJSONRPCInternalMethod *m;
  gchar *complete_method;

  /* Create complete method */
  complete_method = g_strdup_printf ("%s.%s", group, method);

  /* Lock method list access */
  G_LOCK (melo_jsonrpc_mutex);

  /* Create hash table if not yet created */
  if (!melo_jsonrpc_methods) {
    melo_jsonrpc_methods = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                  g_free,
                                                  melo_jsonrpc_free_method);
  }

  /* Method already exists */
  if (g_hash_table_lookup (melo_jsonrpc_methods, complete_method))
    goto failed;

  /* Create new method handler */
  m = g_slice_new (MeloJSONRPCInternalMethod);
  if (!m)
    goto failed;

  /* Fill handler */
  m->params = params;
  m->result = result;
  m->callback = callback;
  m->user_data = user_data;

  /* Add method */
  g_hash_table_insert (melo_jsonrpc_methods, complete_method, m);

  /* Unlock method list access */
  G_UNLOCK (melo_jsonrpc_mutex);

  return TRUE;

failed:
  G_UNLOCK (melo_jsonrpc_mutex);
  g_free (complete_method);
  return FALSE;
}

/**
 * melo_jsonrpc_unregister_method:
 * @group: prefix of the method
 * @method: the method name
 *
 * Unregister one JSON-RPC method named @method and prefixed with @group.
 */
void
melo_jsonrpc_unregister_method (const gchar *group, const gchar *method)
{
  gchar *complete_method;

  /* Create complete method */
  complete_method = g_strdup_printf ("%s.%s", group, method);

  /* Lock method list access */
  G_LOCK (melo_jsonrpc_mutex);

  /* Remove method */
  g_hash_table_remove (melo_jsonrpc_methods, complete_method);

  /* Free hash table if empty */
  if (!g_hash_table_size (melo_jsonrpc_methods)) {
    g_hash_table_unref (melo_jsonrpc_methods);
    melo_jsonrpc_methods = NULL;
  }

  /* Unlock method list access */
  G_UNLOCK (melo_jsonrpc_mutex);

  /* Free complete method */
  g_free (complete_method);
}

/**
 * melo_jsonrpc_register_methods:
 * @group: prefix of the method
 * @methods: an array of #MeloJSONRPCMethod
 * @count: the number of methods in @methods
 *
 * Register multiple JSON-RPC method named with the same prefix @group. All
 * final methods will be as "@group.NAME".
 *
 * Returns: 0 if all methods have been registered or the number of methods which
 * has not been registered.
 */
guint
melo_jsonrpc_register_methods (const gchar *group, MeloJSONRPCMethod *methods,
                               guint count)
{
  JsonParser *parser;
  JsonObject *result;
  JsonArray *params;
  JsonNode *node;
  guint errors = 0, i;
  gboolean ret;

  /* Create a parser */
  parser = json_parser_new ();

  /* Register all methods */
  for (i = 0; i < count; i++) {
    /* Generate params schema */
    if (json_parser_load_from_data (parser, methods[i].params, -1, NULL)) {
      /* Convert string to node */
      node = json_parser_get_root (parser);

      /* Node params type must be an array */
      if (json_node_get_node_type (node) != JSON_NODE_ARRAY)
        continue;

      /* Get array */
      params = json_node_dup_array (node);
    } else
      params = NULL;

    /* Generate result schema */
    if (json_parser_load_from_data (parser, methods[i].result, -1, NULL)) {
      /* Convert string to node */
      node = json_parser_get_root (parser);

      /* Node result type must be an object */
      if (json_node_get_node_type (node) != JSON_NODE_OBJECT)
        continue;

      /* Get array */
      result = json_node_dup_object (node);
    } else
      result = NULL;

    /* Register method */
    ret = melo_jsonrpc_register_method (group, methods[i].method, params,
                                        result, methods[i].callback,
                                        methods[i].user_data);

    /* Failed to register method */
    if (!ret) {
      if (params)
        json_array_unref (params);
      if (result)
        json_object_unref (result);
      errors++;
    }
  }

  /* Release parser */
  g_object_unref (parser);

  return errors;
}

/**
 * melo_jsonrpc_unregister_methods:
 * @group: prefix of the method
 * @methods: an array of #MeloJSONRPCMethod
 * @count: the number of methods in @methods
 *
 * Unregister multiple JSON-RPC method named with the same prefix @group.
 */
void
melo_jsonrpc_unregister_methods (const gchar *group, MeloJSONRPCMethod *methods,
                                 guint count)
{
  guint i;

  /* Unregister all methods */
  for (i = 0; i < count; i++)
    melo_jsonrpc_unregister_method (group, methods[i].method);
}

/* Parse JSON-RPC request */
static JsonNode *
melo_jsonrpc_parse_node (JsonNode *node)
{
  MeloJSONRPCInternalMethod *m;
  MeloJSONRPCCallback callback = NULL;
  gpointer user_data = NULL;
  JsonArray *s_params = NULL;
  JsonNode *result = NULL;
  JsonNode *error = NULL;
  JsonNode *params;
  JsonObject *obj;
  const char *version;
  const char *method;
  const char *id = NULL;
  gint64 nid = -1;

  /* Not an object */
  if (JSON_NODE_TYPE (node) != JSON_NODE_OBJECT)
    goto invalid;

  /* Get object from nide */
  obj = json_node_get_object (node);
  if (!obj)
    goto internal;

  /* Check if jsonrpc is present */
  if (!json_object_has_member (obj, "jsonrpc"))
    goto invalid;

  /* Check JSON-RPC version */
  version = json_object_get_string_member (obj, "jsonrpc");
  if (!version || strcmp (version, "2.0"))
    goto invalid;

  /* Check if method is present */
  if (!json_object_has_member (obj, "method"))
    goto invalid;

  /* Get method */
  method = json_object_get_string_member (obj, "method");
  if (!method)
    goto invalid;

  /* Get params */
  params = json_object_get_member (obj, "params");
  if (params) {
    JsonNodeType type;

    /* Check params type: only object or array allowed */
    type = json_node_get_node_type (params);
    if (type != JSON_NODE_ARRAY && type != JSON_NODE_OBJECT)
      goto invalid;
  }

  /* Get registered method */
  G_LOCK (melo_jsonrpc_mutex);
  if (melo_jsonrpc_methods) {
    m = g_hash_table_lookup (melo_jsonrpc_methods, method);
    if (m) {
      callback = m->callback;
      user_data = m->user_data;
      if (m->params)
        s_params = json_array_ref (m->params);
    }
  }
  G_UNLOCK (melo_jsonrpc_mutex);

  /* Check if id is present */
  if (!json_object_has_member (obj, "id")) {
    /* This is a notification: try to call callback */
    if (callback) {
      callback (method, s_params, params, &result, &error, user_data);
      if (s_params)
        json_array_unref (s_params);
      if (error)
        json_node_free (error);
      if (result)
        json_node_free (result);
    }
    return NULL;
  }

  /* Get id */
  nid = json_object_get_int_member (obj, "id");
  id = json_object_get_string_member (obj, "id");

  /* No callback provided */
  if (!callback)
    goto not_found;

  /* Call user callback */
  callback (method, s_params, params, &result, &error, user_data);
  if (s_params)
    json_array_unref (s_params);

  /* No error or result */
  if (!error && !result)
    goto not_found;

  /* Build response */
  return melo_jsonrpc_build_response_node (result, error, id, nid);

invalid:
  return melo_jsonrpc_build_error (NULL, -1,
                                        MELO_JSONRPC_ERROR_INVALID_REQUEST,
                                        "Invalid request");
not_found:
  return melo_jsonrpc_build_error (id, nid,
                                        MELO_JSONRPC_ERROR_METHOD_NOT_FOUND,
                                        "Method not found");
internal:
  return melo_jsonrpc_build_error (id, -1,
                                        MELO_JSONRPC_ERROR_INTERNAL_ERROR,
                                        "Internal error");
}

/**
 * melo_jsonrpc_parse_request:
 * @request: the JSON-RPC requrest serialized in a string
 * @length: the length og @request, can be -1 for null-terminated string
 * @error: a pointer to a #GError which is set if an error occurred
 *
 * Parse a string @request containing a JSON-RPC serialized request, call the
 * registered callback which match the request method and present the result
 * as a JSON-RPC response serialized in a string.
 * If the method is not registered, a JSON-RPC response is generated with the
 * error MELO_JSONRPC_ERROR_METHOD_NOT_FOUND.
 *
 * Returns: (transfer full): a string containing the serialized #JsonNode
 * corresponding to the respond to the JSON-RPC request. Use g_free() after
 * usage.
 */
gchar *
melo_jsonrpc_parse_request (const gchar *request, gsize length, GError **error)
{
  JsonParser *parser;
  JsonNodeType type;
  JsonNode *req;
  JsonNode *res;
  GError *err = NULL;
  gchar *str;

  /* Create parser */
  parser = json_parser_new ();
  if (!parser)
    return melo_jsonrpc_build_error_str (MELO_JSONRPC_ERROR_INTERNAL_ERROR,
                                         "Internal error");

  /* Parse request */
  if (!json_parser_load_from_data (parser, request, length, &err) ||
      (req = json_parser_get_root (parser)) == NULL) {
    g_clear_error (&err);
    goto parse_error;
  }

  /* Get node type */
  type = json_node_get_node_type (req);

  /* Parse node */
  if (type == JSON_NODE_OBJECT) {
    /* Parse single request */
    res = melo_jsonrpc_parse_node (req);
  } else if (type == JSON_NODE_ARRAY) {
    /* Parse multiple requests: batch */
    JsonArray *req_array;
    JsonArray *res_array;
    JsonNode *node;
    guint count, i;

    /* Get array from node */
    req_array = json_node_get_array (req);
    count = json_array_get_length (req_array);
    if (!count)
      goto invalid;

    /* Create a new array for response */
    res_array = json_array_sized_new (count);
    res = json_node_new (JSON_NODE_ARRAY);
    json_node_take_array (res, res_array);

    /* Parse each elements of array */
    for (i = 0; i < count; i++) {
      /* Get element */
      node = json_array_get_element (req_array, i);

      /* Process requesit */
      node = melo_jsonrpc_parse_node (node);

      /* Add new response to array */
      if (node)
        json_array_add_element (res_array, node);
    }

    /* Check if array is empty */
    count = json_array_get_length (res_array);
    if (!count) {
      json_node_free (res);
      goto empty;
    }
  } else
    goto invalid;

  /* No response */
  if (!res)
    goto empty;

  /* Generate final string */
  str = melo_jsonrpc_node_to_string (res);

  /* Free parser and root node */
  json_node_free (res);
  g_object_unref (parser);

  return str;

parse_error:
  g_object_unref (parser);
  return melo_jsonrpc_build_error_str (MELO_JSONRPC_ERROR_PARSE_ERROR,
                                       "Parse error");
invalid:
  g_object_unref (parser);
  return melo_jsonrpc_build_error_str (MELO_JSONRPC_ERROR_INVALID_REQUEST,
                                       "Invalid request");
empty:
  g_object_unref (parser);
  return NULL;
}

/* Params utils */
static gboolean
melo_jsonrpc_add_node (JsonNode *node, JsonObject *schema,
                       JsonObject *obj, JsonArray *array)
{
  GType vtype = G_TYPE_INVALID;
  const gchar *s_name;
  const gchar *s_type;
  JsonNodeType type;

  /* Get name and type from schema */
  s_name = json_object_get_string_member (schema, "name");
  s_type = json_object_get_string_member (schema, "type");
  if (!s_name || !s_type)
    return FALSE;

  /* Get type */
  type = json_node_get_node_type (node);
  if (type == JSON_NODE_VALUE)
    vtype = json_node_get_value_type (node);

  /* Check type:
   * We check only first letter of the type string.
   */
  switch (s_type[0]) {
    case 'b':
      /* Boolean: check type */
      if (vtype != G_TYPE_BOOLEAN)
        return FALSE;

      /* Add to object / array */
      if (obj || array) {
        gboolean v;
        v = json_node_get_boolean (node);
        if (obj)
          json_object_set_boolean_member (obj, s_name, v);
        else
          json_array_add_boolean_element (array, v);
        break;
      }
      break;
    case 'i':
      /* Integer: check type */
      if (vtype != G_TYPE_INT64)
        return FALSE;

      /* Add to object / array */
      if (obj || array) {
        gint64 v;
        v = json_node_get_int (node);
        if (obj)
          json_object_set_int_member (obj, s_name, v);
        else
          json_array_add_int_element (array, v);
      }
      break;
    case 'd':
      /* Double: check type */
      if (vtype != G_TYPE_DOUBLE)
        return FALSE;

      /* Add to object / array */
      if (obj || array) {
        gdouble v;
        v = json_node_get_double (node);
        if (obj)
          json_object_set_double_member (obj, s_name, v);
        else
          json_array_add_double_element (array, v);
      }
      break;
    case 's':
      /* String: check type */
      if (vtype != G_TYPE_STRING)
        return FALSE;

      /* Add to object / array */
      if (obj || array) {
        const gchar *v;
        v = json_node_get_string (node);
        if (obj)
          json_object_set_string_member (obj, s_name, v);
        else
          json_array_add_string_element (array, v);
      }
      break;
    case 'o':
      /* Object: check type */
      if (type != JSON_NODE_OBJECT)
        return FALSE;

      /* Add to object / array */
      if (obj || array) {
        JsonObject *v;
        v = json_node_dup_object (node);
        if (obj)
          json_object_set_object_member (obj, s_name, v);
        else
          json_array_add_object_element (array, v);
      }
      break;
    case 'a':
      /* Array: check type */
      if (type != JSON_NODE_ARRAY)
        return FALSE;

      /* Add to object / array */
      if (obj || array) {
        JsonArray *v;
        v = json_node_dup_array (node);
        if (obj)
          json_object_set_array_member (obj, s_name, v);
        else
          json_array_add_array_element (array, v);
      }
      break;
    default:
      return FALSE;
  }
  return TRUE;
}

static gboolean
melo_jsonrpc_get_json_node (JsonArray *schema_params, JsonNode *params,
                            JsonObject *obj, JsonArray *array,
                            JsonNode **error)
{
  JsonObject *schema;
  JsonNodeType type;
  JsonNode *node;
  guint count, i;

  /* Check schema */
  if (!schema_params)
    return FALSE;

  /* No params to check */
  if (!params) {
    if (error && *error == NULL)
      *error = melo_jsonrpc_build_error_node (
                                             MELO_JSONRPC_ERROR_INVALID_REQUEST,
                                             "Invalid request");
    return FALSE;
  }

  /* Get element count from schema */
  count = json_array_get_length (schema_params);

  /* Get type */
  type = json_node_get_node_type (params);

  /* Already an object */
  if (type == JSON_NODE_OBJECT) {
    const gchar *name;
    JsonObject *o;

    /* Get object */
    o = json_node_get_object (params);

    /* Parse object */
    for (i = 0; i < count; i++) {
      /* Get next schema object */
      schema = json_array_get_object_element (schema_params, i);
      if (!schema)
        goto failed;

      /* Get parameter name */
      name = json_object_get_string_member (schema, "name");
      if (!name)
        goto failed;

      /* Get node */
      node = json_object_get_member (o, name);
      if (!node) {
        /* Get required flag: failed if not defined or TRUE */
        node = json_object_get_member (schema, "required");
        if (!node || (node && json_node_get_boolean (node)))
          goto failed;

        /* When not required:
         *  - skip when converting to an object,
         *  - stop when converting to an array.
         */
        if (obj)
            continue;
        if (array)
        return TRUE;
      }

      /* Check node type */
      if (!melo_jsonrpc_add_node (node, schema, obj, array))
        goto failed;
    }
  } else if (type == JSON_NODE_ARRAY) {
    guint params_count;
    JsonArray *a;

    /* Get array */
    a = json_node_get_array (params);
    params_count = json_array_get_length (a);

    /* Parse object */
    for (i = 0; i < count; i++) {
      /* Get next schema object */
      schema = json_array_get_object_element (schema_params, i);
      if (!schema)
        goto failed;

      /* No more parameters available */
      if (i >= params_count) {
        /* Get required flag: failed if not defined or TRUE */
        node = json_object_get_member (schema, "required");
        if (!node || (node && json_node_get_boolean (node)))
          goto failed;

        /* If this parameter was not required: stop conversion */
        return TRUE;
      }

      /* Get node */
      node = json_array_get_element (a, i);
      if (!node)
        goto failed;

      /* Check node type */
      if (!melo_jsonrpc_add_node (node, schema, obj, array))
        goto failed;
    }
  }
  return TRUE;

failed:
  if (error && *error == NULL)
    *error = melo_jsonrpc_build_error_node (MELO_JSONRPC_ERROR_INVALID_PARAMS,
                                            "Invalid params");
  return FALSE;
}

/**
 * melo_jsonrpc_check_params:
 * @schema_params: the schema to use for parameters checking (see
 *    #MeloJSONRPCMethod for more details)
 * @params: the parameters to check
 * @error: a pointer to a #JsonNode which is set with a valid JSON-RPC error if
 *    an error has occurred
 *
 * Check if JSON-RPC parameters in @params are matching the schema in
 * @schema_params.
 *
 * Returns: %TRUE if the parameters are valid, %FALSE otherwise.
 */
gboolean
melo_jsonrpc_check_params (JsonArray *schema_params, JsonNode *params,
                           JsonNode **error)
{
  return melo_jsonrpc_get_json_node (schema_params, params, NULL, NULL, error);
}

/**
 * melo_jsonrpc_get_object:
 * @schema_params: the schema to use for parameters conversion (see
 *    #MeloJSONRPCMethod for more details)
 * @params: the parameters to convert
 * @error: a pointer to a #JsonNode which is set with a valid JSON-RPC error if
 *    an error has occurred
 *
 * Convert the #JsonNode containing the parameters into a #JsonObject with all
 * members corresponding to each parameters.
 *
 * Returns: (transfer full): a new #JsonObject containing all parameters sorted
 * by name, or %NULL if an error occurred. Use json_object_unref() after usage.
 */
JsonObject *
melo_jsonrpc_get_object (JsonArray *schema_params, JsonNode *params,
                         JsonNode **error)
{
  JsonObject *obj;

  /* Allocate new object */
  obj = json_object_new ();

  /* Get node */
  if (!melo_jsonrpc_get_json_node (schema_params, params, obj, NULL, error)) {
    json_object_unref (obj);
    return NULL;
  }

  return obj;
}

/**
 * melo_jsonrpc_get_array:
 * @schema_params: the schema to use for parameters conversion (see
 *    #MeloJSONRPCMethod for more details)
 * @params: the parameters to convert
 * @error: a pointer to a #JsonNode which is set with a valid JSON-RPC error if
 *    an error has occurred
 *
 * Convert the #JsonNode containing the parameters into a #JsonArray with all
 * items sorted as the provided schema.
 *
 * Returns: (transfer full): a new #JsonArray containing all parameters sorted
 * as provided schema, or %NULL if an error occurred. Use json_array_unref()
 * after usage.
 */
JsonArray *
melo_jsonrpc_get_array (JsonArray *schema_params, JsonNode *params,
                        JsonNode **error)
{
  JsonArray *array;
  guint count;

  /* Get array length */
  count = json_array_get_length (schema_params);

  /* Allocate new array */
  array = json_array_sized_new (count);

  /* Get array */
  if (!melo_jsonrpc_get_json_node (schema_params, params, NULL, array, error)) {
    json_array_unref (array);
    return NULL;
  }

  return array;
}

/* Helpers */
static gchar *
melo_jsonrpc_node_to_string (JsonNode *node)
{
  JsonGenerator *gen;
  gchar *str;

  /* Create a new generator */
  gen = json_generator_new ();
  if (!gen)
    return NULL;

  /* Set root node */
  json_generator_set_root (gen, node);

  /* Generate string */
  str = json_generator_to_data (gen, NULL);

  /* Free generator */
  g_object_unref (gen);

  return str;
}

static JsonNode *
melo_jsonrpc_build_response_node (JsonNode *result, JsonNode *error,
                                  const gchar *id, gint64 nid)
{
  JsonBuilder *builder;
  JsonNode *node;

  /* Create new builder */
  builder = json_builder_new ();
  if (!builder)
    return NULL;

  /* Begin a new object */
  json_builder_begin_object (builder);

  /* Add jsonrpc member */
  json_builder_set_member_name (builder, "jsonrpc");
  json_builder_add_string_value (builder, "2.0");

  /* Set result or error */
  if (error) {
    /* Add error member */
    json_builder_set_member_name (builder, "error");
    json_builder_add_value (builder, error);

    /* Free result if exists */
    if (result)
      json_node_free (result);
  } else if (result) {
    /* Add result member */
    json_builder_set_member_name (builder, "result");
    json_builder_add_value (builder, result);
  }

  /* Add id member: we assume ID cannot be negative */
  json_builder_set_member_name (builder, "id");
  if (nid < 0 || id)
    json_builder_add_string_value (builder, id);
  else
    json_builder_add_int_value (builder, nid);

  json_builder_end_object (builder);

  /* Get final object */
  node = json_builder_get_root (builder);

  /* Free builder */
  g_object_unref (builder);

  return node;
}

static JsonNode *
melo_jsonrpc_build_error_nodev (MeloJSONRPCError error_code,
                                const char *error_format, va_list args)
{
  gchar *error_message;
  JsonObject *obj;
  JsonNode *node;

  /* Create a new JSON builder */
  obj = json_object_new ();
  if (!obj)
    return NULL;

  /* Generate error message */
  error_message = g_strdup_vprintf (error_format, args);

  /* Begin a new object */
  json_object_set_int_member (obj, "code", error_code);
  json_object_set_string_member (obj, "message", error_message);

  /* Free message */
  g_free (error_message);

  /* Create node */
  node = json_node_new (JSON_NODE_OBJECT);
  json_node_take_object (node, obj);

  return node;
}

static JsonNode *
melo_jsonrpc_build_error (const char *id, gint64 nid,
                          MeloJSONRPCError error_code,
                          const char *error_format, ...)
{
  JsonNode *node;
  va_list args;

  /* Generate error node */
  va_start (args, error_format);
  node = melo_jsonrpc_build_error_nodev (error_code, error_format, args);
  va_end (args);

  /* Generate final response */
  return melo_jsonrpc_build_response_node (NULL, node, id, nid);
}

static gchar *
melo_jsonrpc_build_error_str (MeloJSONRPCError error_code,
                              const char *error_format, ...)
{
  JsonNode *node;
  va_list args;
  gchar *str;

  /* Generate error node */
  va_start (args, error_format);
  node = melo_jsonrpc_build_error_nodev (error_code, error_format, args);
  va_end (args);

  /* Generate final response */
  node = melo_jsonrpc_build_response_node (NULL, node, NULL, -1);

  /* Generate string */
  str = melo_jsonrpc_node_to_string (node);
  json_node_free (node);

  return str;
}

/**
 * melo_jsonrpc_build_error_node:
 * @error_code: the JSON-RPC error code
 * @error_format: the string format. See the printf() documentation
 * @...: the parameters to insert into the format string
 *
 * Generate a JSON-RPC response with @error set.
 *
 * Returns: (transfer full): a new #JsonNode containing the JSON-RPC response
 * with @error set, or %NULL if an error occurred. Use json_node_unref() after
 * usage.
 */
JsonNode *
melo_jsonrpc_build_error_node (MeloJSONRPCError error_code,
                               const char *error_format, ...)
{
  JsonNode *node;
  va_list args;

  /* Generate error node */
  va_start (args, error_format);
  node = melo_jsonrpc_build_error_nodev (error_code, error_format, args);
  va_end (args);

  return node;
}
