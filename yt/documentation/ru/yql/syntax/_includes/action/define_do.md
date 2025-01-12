---
vcsPath: ydb/docs/ru/core/yql/reference/yql-core/syntax/_includes/action/define_do.md
sourcePath: ydb/docs/ru/core/yql/reference/yql-core/syntax/_includes/action/define_do.md
---
## DEFINE ACTION {#define-action}

Задает именованное действие, которое представляют собой параметризуемый блок из нескольких выражений верхнего уровня.

**Синтаксис**

1. `DEFINE ACTION` — объявление действия.
1. [Имя действия](../../expressions.md#named-nodes), по которому объявляемое действие доступно далее для вызова.
1. В круглых скобках — список имен параметров.
1. Ключевое слово `AS`.
1. Список выражений верхнего уровня.
1. `END DEFINE` — маркер последнего выражения внутри действия.

Один или более последних параметров могут быть помечены знаком вопроса `?` как необязательные. Если они не будут указаны при вызове, то им будет присвоено значение `NULL`.

## DO {#do}

Выполняет `ACTION` с указанными параметрами.

**Синтаксис**
1. `DO` — выполнение действия.
1. Именованное выражение, по которому объявлено действие.
1. В круглых скобках — список значений для использования в роли параметров.

`EMPTY_ACTION` — действие, которое ничего не выполняет.

 <!-- In fact, if user file system integration is supported in the product. YQL service over YDB may also be here. -->

{% note info "Примечание" %}

В больших запросах объявление действий можно выносить в отдельные файлы и подключать их в основной запрос с помощью [EXPORT](../../export_import.md#export) + [IMPORT](../../export_import.md#import), чтобы вместо одного длинного текста получилось несколько логических частей, в которых проще ориентироваться. Важный нюанс: директива `USE my_cluster;` в импортирующем запросе не влияет на поведение объявленных в других файлах действий.

{% endnote %}


**Пример**

```yql
DEFINE ACTION $hello_world($name, $suffix?) AS
    $name = $name ?? ($suffix ?? "world");
    SELECT "Hello, " || $name || "!";
END DEFINE;

DO EMPTY_ACTION();
DO $hello_world(NULL);
DO $hello_world("John");
DO $hello_world(NULL, "Earth");
```

