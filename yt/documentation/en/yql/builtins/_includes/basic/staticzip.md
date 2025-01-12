---
vcsPath: ydb/docs/ru/core/yql/reference/yql-core/builtins/_includes/basic/staticzip.md
sourcePath: ydb/docs/ru/core/yql/reference/yql-core/builtins/_includes/basic/staticzip.md
---
## StaticZip

Merges structures or tuples element-by-element. All arguments (one or more) must be either structures with the same set of fields or tuples of the same length.
The result will be a structure or tuple, respectively.
Each item of the result is a tuple comprised of items taken from arguments.

**Signature**
```
StaticZip(Struct, Struct)->Struct
StaticZip(Tuple, Tuple)->Tuple
```

**Examples:**
```yql
$one = <|k1:1, k2:2.0|>;
$two = <|k1:3.0, k2:4|>;

-- поэлементное сложение двух структур
SELECT StaticMap(StaticZip($one, $two), ($tuple)->($tuple.0 + $tuple.1)) AS sum;
```

