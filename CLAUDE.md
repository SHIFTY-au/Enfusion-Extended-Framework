# Enfusion Extended Framework — Claude Notes

## Language: Enforce Script

All `.c` files in this project are **Enforce Script** (Arma Reforger's scripting language), not C or C++. Enforce Script has several important restrictions compared to C-family languages:

### Syntax restrictions

- **No ternary operator.** `x ? a : b` is NOT valid. Always use `if/else` instead:
  ```
  // WRONG
  float val = condition ? a : b;

  // CORRECT
  float val;
  if (condition)
      val = a;
  else
      val = b;
  ```

- **No array assignment by value.** You cannot do `arr1 = arr2` for fixed-size arrays (e.g. `vector[4]`). Copy element by element:
  ```
  dst[0] = src[0];
  dst[1] = src[1];
  dst[2] = src[2];
  dst[3] = src[3];
  ```

- **No lambda / anonymous functions.** Use named methods and `Insert()` on `ScriptInvoker`.

- **`ref` keyword required for heap-allocated objects** (arrays, class instances stored as fields).

### ScriptInvoker usage

Use untyped `ScriptInvoker` for script events. Document the expected callback signature in a comment:

```
// Callback signature: void Fn(IEntity heli)
protected ref ScriptInvoker m_OnHelicopterSpawned = new ScriptInvoker();

// Fire:
m_OnHelicopterSpawned.Invoke(heli);

// Subscribe:
m_OnHelicopterSpawned.Insert(MyCallback);
```

### General patterns

- Server-authority checks go at the top of every public method: `if (!Replication.IsServer()) return;`
- Use `GetGame().GetCallqueue().CallLater(Method, delayMs, repeat)` for deferred/periodic calls.
- Entity deletion: `SCR_EntityHelper.DeleteEntityAndChildren(entity)`.
- Components on the same entity: `owner.FindComponent(MyComponent)`.
