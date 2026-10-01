#pragma once

#import <assert.h>
#import <js.h>
#import <stdint.h>
#import <stdlib.h>

#import <CoreFoundation/CoreFoundation.h>
#import <Foundation/Foundation.h>
#import <objc/runtime.h>

static const js_type_tag_t bare_foundation__carrier = {0x9e2f4a1c7b3d5e60, 0xc1a8d34f6b920e75};

typedef struct {
  CFTypeRef object;
  js_ref_t *wrapper;
  uint32_t claims;
} bare_foundation_entry_t;

typedef struct {
  uint32_t tag;
  js_ref_t *wrapper;
} bare_foundation__claim_t;

typedef struct {
  CFMutableDictionaryRef entries;
  CFMutableDictionaryRef tags;

  uint32_t next_tag;
  uint32_t refs;
} bare_foundation_registry_t;

/**
 * Keep `registry` alive, for example while native code holds on to it. Give the
 * reference back with `bare_foundation_registry_release()`.
 */
static void
bare_foundation_registry_retain(bare_foundation_registry_t *registry) {
  registry->refs++;
}

static void
bare_foundation__release_entry(const void *key, const void *value, void *context) {
  bare_foundation_entry_t *entry = (bare_foundation_entry_t *) value;

  CFRelease(entry->object);

  free(entry);
}

/**
 * Give back a reference to `registry`. The registry is freed when the last
 * reference is gone. Call this from a finalizer rather than a teardown
 * callback, because releasing an object can run a `dealloc` that calls back
 * into JavaScript.
 */
static void
bare_foundation_registry_release(bare_foundation_registry_t *registry) {
  assert(registry->refs > 0);

  if (--registry->refs > 0) return;

  CFDictionaryApplyFunction(registry->entries, bare_foundation__release_entry, NULL);

  CFRelease(registry->entries);
  CFRelease(registry->tags);

  free(registry);
}

static void
bare_foundation__on_registry_release(js_env_t *env, void *data, void *finalize_hint) {
  bare_foundation_registry_release(data);
}

/**
 * Create a registry for an addon, and pass it as the data pointer of every
 * function the addon exports. The registry lives as long as `exports` and every
 * token it has handed out.
 */
static bare_foundation_registry_t *
bare_foundation_registry_create(js_env_t *env, js_value_t *exports) {
  int err;

  bare_foundation_registry_t *registry = malloc(sizeof(bare_foundation_registry_t));

  registry->entries = CFDictionaryCreateMutable(NULL, 0, NULL, NULL);
  registry->tags = CFDictionaryCreateMutable(NULL, 0, NULL, NULL);

  registry->next_tag = 1;
  registry->refs = 1;

  err = js_add_finalizer(env, exports, registry, bare_foundation__on_registry_release, NULL, NULL);
  assert(err == 0);

  return registry;
}

/**
 * Give `object` a tag and return it, or 0 if `object` is `nil`. An object that
 * already has a tag keeps it.
 */
static uint32_t
bare_foundation_tag(bare_foundation_registry_t *registry, id object) {
  if (object == nil) return 0;

  const void *existing;

  if (CFDictionaryGetValueIfPresent(registry->tags, (__bridge const void *) object, &existing)) {
    return (uint32_t) (uintptr_t) existing;
  }

  uint32_t tag = registry->next_tag++;

  bare_foundation_entry_t *entry = malloc(sizeof(bare_foundation_entry_t));

  entry->object = CFRetain((__bridge CFTypeRef) object);
  entry->wrapper = NULL;
  entry->claims = 0;

  CFDictionarySetValue(registry->entries, (const void *) (uintptr_t) tag, entry);
  CFDictionarySetValue(registry->tags, (__bridge const void *) object, (const void *) (uintptr_t) tag);

  return tag;
}

static bare_foundation_entry_t *
bare_foundation__entry(bare_foundation_registry_t *registry, uint32_t tag) {
  return (bare_foundation_entry_t *) CFDictionaryGetValue(registry->entries, (const void *) (uintptr_t) tag);
}

static js_value_t *
bare_foundation__wrapper(js_env_t *env, bare_foundation_entry_t *entry) {
  if (entry->wrapper == NULL) return NULL;

  js_value_t *result;
  int err = js_get_reference_value(env, entry->wrapper, &result);
  assert(err == 0);

  return result;
}

/**
 * Return the object for `tag`, or `nil` if there is none.
 */
static id
bare_foundation_object(bare_foundation_registry_t *registry, uint32_t tag) {
  bare_foundation_entry_t *entry = bare_foundation__entry(registry, tag);

  if (entry == NULL) return nil;

  return (__bridge id) entry->object;
}

/**
 * Give `object` a tag and return it as a JavaScript number, or `null` if
 * `object` is `nil`.
 */
static js_value_t *
bare_foundation_bridge(js_env_t *env, bare_foundation_registry_t *registry, id object) {
  int err;

  js_value_t *result;

  if (object == nil) {
    err = js_get_null(env, &result);
    assert(err == 0);
  } else {
    err = js_create_uint32(env, bare_foundation_tag(registry, object), &result);
    assert(err == 0);
  }

  return result;
}

/**
 * Return the object for the tag in `value`, or `nil` if `value` is not a number
 * or not a known tag.
 */
static id
bare_foundation_to_object(js_env_t *env, bare_foundation_registry_t *registry, js_value_t *value) {
  int err;

  bool is;
  err = js_is_number(env, value, &is);
  assert(err == 0);

  if (!is) return nil;

  uint32_t tag;
  err = js_get_value_uint32(env, value, &tag);
  assert(err == 0);

  return bare_foundation_object(registry, tag);
}

static int
bare_foundation__read_uint32(js_env_t *env, js_value_t *value, const char *name, uint32_t *result) {
  int err;

  bool is;
  err = js_is_number(env, value, &is);
  assert(err == 0);

  if (!is) {
    err = js_throw_type_errorf(env, NULL, "Expected '%s' to be a number", name);
    assert(err == 0);

    return -1;
  }

  err = js_get_value_uint32(env, value, result);
  assert(err == 0);

  return 0;
}

/**
 * Read the object for the tag in `value` into `result` and return 0. If `value`
 * is not a number or not a known tag, throw a JavaScript error that mentions
 * `name` and return -1.
 */
static int
bare_foundation_read_tag(js_env_t *env, bare_foundation_registry_t *registry, js_value_t *value, const char *name, void **result) {
  int err;

  uint32_t tag;
  err = bare_foundation__read_uint32(env, value, name, &tag);
  if (err < 0) return err;

  bare_foundation_entry_t *entry = bare_foundation__entry(registry, tag);

  if (entry == NULL) {
    err = js_throw_errorf(env, NULL, "Unknown tag %u", tag);
    assert(err == 0);

    return -1;
  }

  *result = (void *) entry->object;

  return 0;
}

/**
 * Like `bare_foundation_read_tag()`, but also throw a `TypeError` and return -1
 * if the object is not a `type`.
 */
static int
bare_foundation_read_type(js_env_t *env, bare_foundation_registry_t *registry, js_value_t *value, const char *name, Class type, void **result) {
  int err;

  void *object;
  err = bare_foundation_read_tag(env, registry, value, name, &object);
  if (err < 0) return err;

  if (![(__bridge id) object isKindOfClass:type]) {
    err = js_throw_type_errorf(env, NULL, "Expected '%s' to be a %s, not a %s", name, class_getName(type), object_getClassName((__bridge id) object));
    assert(err == 0);

    return -1;
  }

  *result = object;

  return 0;
}

/**
 * Return the JavaScript wrapper of `object`, or `NULL` if it has none.
 */
static js_value_t *
bare_foundation_lookup(js_env_t *env, bare_foundation_registry_t *registry, id object) {
  const void *tag;

  if (!CFDictionaryGetValueIfPresent(registry->tags, (__bridge const void *) object, &tag)) return NULL;

  bare_foundation_entry_t *entry = bare_foundation__entry(registry, (uint32_t) (uintptr_t) tag);

  if (entry == NULL) return NULL;

  return bare_foundation__wrapper(env, entry);
}

static void
bare_foundation__on_token_finalize(js_env_t *env, void *data, void *finalize_hint) {
  int err;

  bare_foundation_registry_t *registry = finalize_hint;

  bare_foundation__claim_t *claim = (bare_foundation__claim_t *) data;

  uint32_t tag = claim->tag;

  err = js_delete_reference(env, claim->wrapper);
  assert(err == 0);

  bare_foundation_entry_t *entry = bare_foundation__entry(registry, tag);

  if (entry->wrapper == claim->wrapper) entry->wrapper = NULL;

  free(claim);

  if (--entry->claims == 0) {
    CFTypeRef object = entry->object;

    CFDictionaryRemoveValue(registry->tags, object);
    CFDictionaryRemoveValue(registry->entries, (const void *) (uintptr_t) tag);

    free(entry);

    CFRelease(object);
  }

  bare_foundation_registry_release(registry);
}

/**
 * Export as `claim(tag, wrapper)`. Return a token that keeps the object alive
 * until the token is garbage collected. A tag can be claimed more than once,
 * and the object stays alive until every token is gone. The registry does not
 * keep any wrapper alive.
 */
static js_value_t *
bare_foundation_claim(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 2;
  js_value_t *argv[2];

  bare_foundation_registry_t *registry;
  err = js_get_callback_info(env, info, &argc, argv, NULL, (void **) &registry);
  assert(err == 0);

  assert(argc == 2);

  uint32_t tag;
  err = bare_foundation__read_uint32(env, argv[0], "tag", &tag);
  if (err < 0) return NULL;

  bare_foundation_entry_t *entry = bare_foundation__entry(registry, tag);

  if (entry == NULL) {
    err = js_throw_errorf(env, NULL, "Unknown tag %u", tag);
    assert(err == 0);

    return NULL;
  }

  bare_foundation__claim_t *claim = malloc(sizeof(bare_foundation__claim_t));

  claim->tag = tag;

  err = js_create_reference(env, argv[1], 0, &claim->wrapper);
  assert(err == 0);

  if (bare_foundation__wrapper(env, entry) == NULL) entry->wrapper = claim->wrapper;

  entry->claims++;

  bare_foundation_registry_retain(registry);

  js_value_t *token;
  err = js_create_external(env, claim, bare_foundation__on_token_finalize, registry, &token);
  assert(err == 0);

  return token;
}

/**
 * Export as `wrapper(tag)`. Return the first wrapper of `tag` that is still
 * alive, or `null` if there is none. An unknown tag is not an error, because
 * `adopt()` asks with a tag that may belong to another addon to find out
 * whether an object is ours.
 */
static js_value_t *
bare_foundation_wrapper(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 1;
  js_value_t *argv[1];

  bare_foundation_registry_t *registry;
  err = js_get_callback_info(env, info, &argc, argv, NULL, (void **) &registry);
  assert(err == 0);

  assert(argc == 1);

  uint32_t tag;
  err = bare_foundation__read_uint32(env, argv[0], "tag", &tag);
  if (err < 0) return NULL;

  bare_foundation_entry_t *entry = bare_foundation__entry(registry, tag);

  js_value_t *result = entry == NULL ? NULL : bare_foundation__wrapper(env, entry);

  if (result == NULL) {
    err = js_get_null(env, &result);
    assert(err == 0);
  }

  return result;
}

/**
 * Export as `registrySize()`. Return the number of objects in the registry.
 * Useful for finding leaks in tests.
 */
static js_value_t *
bare_foundation_registry_size(js_env_t *env, js_callback_info_t *info) {
  int err;

  bare_foundation_registry_t *registry;
  err = js_get_callback_info(env, info, NULL, NULL, NULL, (void **) &registry);
  assert(err == 0);

  js_value_t *result;
  err = js_create_uint32(env, (uint32_t) CFDictionaryGetCount(registry->entries), &result);
  assert(err == 0);

  return result;
}

static void
bare_foundation__on_carrier_finalize(js_env_t *env, void *data, void *finalize_hint) {
  CFRelease(data);
}

/**
 * Export as `handle(tag)`. Return a handle that another addon can adopt. The
 * handle holds its own reference to the object.
 */
static js_value_t *
bare_foundation_handle(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 1;
  js_value_t *argv[1];

  bare_foundation_registry_t *registry;
  err = js_get_callback_info(env, info, &argc, argv, NULL, (void **) &registry);
  assert(err == 0);

  assert(argc == 1);

  void *object;
  err = bare_foundation_read_tag(env, registry, argv[0], "tag", &object);
  if (err < 0) return NULL;

  js_value_t *carrier;
  err = js_create_object(env, &carrier);
  assert(err == 0);

  err = js_wrap(env, carrier, (void *) CFRetain(object), bare_foundation__on_carrier_finalize, NULL, NULL);
  assert(err == 0);

  err = js_add_type_tag(env, carrier, &bare_foundation__carrier);
  assert(err == 0);

  return carrier;
}

/**
 * Export as `adopt(handle)`. Return a tag for the object in `handle`. A object
 * that already has a tag keeps it.
 */
static js_value_t *
bare_foundation_adopt(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 1;
  js_value_t *argv[1];

  bare_foundation_registry_t *registry;
  err = js_get_callback_info(env, info, &argc, argv, NULL, (void **) &registry);
  assert(err == 0);

  assert(argc == 1);

  bool is;
  err = js_is_object(env, argv[0], &is);
  assert(err == 0);

  if (is) {
    err = js_check_type_tag(env, argv[0], &bare_foundation__carrier, &is);
    assert(err == 0);
  }

  if (!is) {
    err = js_throw_type_error(env, NULL, "Expected 'carrier' to be a Foundation handle");
    assert(err == 0);

    return NULL;
  }

  void *object;
  err = js_unwrap(env, argv[0], &object);
  assert(err == 0);

  js_value_t *result;
  err = js_create_uint32(env, bare_foundation_tag(registry, (__bridge id) object), &result);
  assert(err == 0);

  return result;
}
