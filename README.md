# bare-foundation-registry

Keep track of the Foundation objects that a Bare addon gives to JavaScript, and pass them from one addon to another.

An addon does not give JavaScript a pointer to an object. It gives it a number, called a tag. The registry remembers which object each tag stands for and which JavaScript object wraps it, and it keeps the object alive for as long as that wrapper is alive.

Every addon has its own registry, so a tag only means something to the addon that made it. To pass an object to another addon, you turn it into a handle, and the other addon adopts the handle into its own registry.

The module has two parts: `registry.h` for the native code of an addon, and a JavaScript module for its JavaScript code.

```
npm i bare-foundation-registry
```

## Usage

### Setting up an addon

Add the header to the include path of the addon in `CMakeLists.txt`:

```cmake
target_include_directories(
  ${my_addon}
  PRIVATE
    node_modules/bare-foundation-registry/lib
)
```

Create a registry when the addon loads, and pass it as the data pointer of every function the addon exports. Export the five functions of the registry next to your own:

```objc
#import "registry.h"

static js_value_t *
my_addon_exports(js_env_t *env, js_value_t *exports) {
  int err;

  bare_foundation_registry_t *registry = bare_foundation_registry_create(env, exports);

#define V(name, fn) \
  { \
    js_value_t *val; \
    err = js_create_function(env, name, -1, fn, registry, &val); \
    assert(err == 0); \
    err = js_set_named_property(env, exports, name, val); \
    assert(err == 0); \
  }

  V("claim", bare_foundation_claim)
  V("wrapper", bare_foundation_wrapper)
  V("handle", bare_foundation_handle)
  V("adopt", bare_foundation_adopt)
  V("registrySize", bare_foundation_registry_size)

  // Your own functions go here

#undef V

  return exports;
}
```

### Giving Foundation objects to JavaScript

Your functions get the registry back from their data pointer. Give an object a tag before you return it, and turn a tag back into an object when JavaScript passes one in:

```objc
#import <js.h>
#import <AppKit/AppKit.h>

#import "registry.h"

static js_value_t *
my_addon_red(js_env_t *env, js_callback_info_t *info) {
  int err;

  bare_foundation_registry_t *registry;
  err = js_get_callback_info(env, info, NULL, NULL, NULL, (void **) &registry);
  assert(err == 0);

  return bare_foundation_bridge(env, registry, [NSColor redColor]);
}

static js_value_t *
my_addon_set_background(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 2;
  js_value_t *argv[2];

  bare_foundation_registry_t *registry;
  err = js_get_callback_info(env, info, &argc, argv, NULL, (void **) &registry);
  assert(err == 0);

  void *box;
  err = bare_foundation_read_type(env, registry, argv[0], "box", [NSBox class], &box);
  if (err < 0) return NULL;

  void *color;
  err = bare_foundation_read_type(env, registry, argv[1], "color", [NSColor class], &color);
  if (err < 0) return NULL;

  ((__bridge NSBox *) box).fillColor = (__bridge NSColor *) color;

  return NULL;
}
```

### Wrapping Foundation objects in JavaScript

On the JavaScript side, wrap each tag in an object and claim it. Claiming tells the registry which object wraps the tag, and returns a token that keeps the object alive until the wrapper is garbage collected.

The wrapper gives its tag to the registry through the `registry.tag` symbol. Where it keeps the tag is up to you:

```js
const binding = require('./binding')
const registry = require('bare-foundation-registry')

class Label {
  constructor(tag) {
    this._tag = tag
    this._token = binding.claim(tag, this)
  }

  get [registry.tag]() {
    return this._tag
  }
}
```

When the addon returns a tag that JavaScript has seen before, reuse the wrapper it already has:

```js
function wrap(tag) {
  if (tag === null) return null

  return binding.wrapper(tag) || new Label(tag)
}
```

### Passing Foundation objects between addons

A wrapper that other addons may use also gives out a handle through the `registry.handle` symbol. A handle carries the object itself rather than a tag, so another addon can take it into its own registry:

```js
class Label {
  // ...

  get [registry.handle]() {
    return binding.handle(this._tag)
  }
}
```

An addon that takes an object from JavaScript adopts it before it passes it to native code. Adopting returns a tag in its own registry, whichever addon the object came from:

```js
const binding = require('./binding')
const registry = require('bare-foundation-registry')

class Box {
  set child(child) {
    binding.boxSetChild(this._tag, registry.adopt(binding, child))
  }
}
```

## License

Apache-2.0
