---
slug: /ru/sql-reference/aggregate-functions/reference/quantiletiming
sidebar_position: 204
---

# quantileTiming {#quantiletiming}

Вычисляет [квантиль](https://ru.wikipedia.org/wiki/Квантиль) числовой последовательности с детерминированной точностью.

Результат детерминирован (не зависит от порядка обработки запроса). Функция оптимизирована для работы с последовательностями, описывающими такие распределения, как время загрузки веб-страниц или время отклика бэкенда.

Внутренние состояния функций `quantile*` не объединяются, если они используются в одном запросе. Если вам необходимо вычислить квантили нескольких уровней, используйте функцию [quantiles](/ru/sql-reference/aggregate-functions/reference/quantiles), это повысит эффективность запроса.

**Синтаксис**

``` sql
quantileTiming(level)(expr)
```

Алиас: `medianTiming`.

**Аргументы**

-   `level` — уровень квантили. Опционально. Константное значение с плавающей запятой от 0 до 1. Мы рекомендуем использовать значение `level` из диапазона `[0.01, 0.99]`. Значение по умолчанию: 0.5. При `level=0.5` функция вычисляет [медиану](https://ru.wikipedia.org/wiki/Медиана_(статистика)).

-   `expr` — [выражение](../../syntax.md#syntax-expressions), зависящее от значений столбцов, возвращающее данные типа [Float\*](../../../sql-reference/data-types/float.md).

        - Если в функцию передать отрицательные значения, то её поведение не определено.
        - Если значение больше, чем 30 000 (например, время загрузки страницы превышает 30 секунд), то оно приравнивается к 30 000.

**Точность**

Вычисления точны при соблюдении следующих условий:

-   Размер выборки не превышает 5670 элементов.
-   Размер выборки превышает 5670 элементов, но значение каждого элемента не больше 1024.

В противном случае, результат вычисления округляется до ближайшего множителя числа 16.

:::note Примечание
Для указанного типа последовательностей функция производительнее и точнее, чем [quantile](/ru/sql-reference/aggregate-functions/reference/quantile).
:::

**Возвращаемое значение**

-   Квантиль заданного уровня.

Тип: `Float32`.

:::note Примечание
Если в функцию `quantileTimingIf` не передать значений, то вернётся [NaN](../../../sql-reference/data-types/float.md#data_type-float-nan-inf). Это необходимо для отделения подобных случаев от случаев, когда результат 0. Подробности про сортировку `NaN` cмотрите в разделе [Секция ORDER BY](../../../sql-reference/statements/select/order-by.md#select-order-by).
:::

**Пример**

Входная таблица:

``` text
┌─response_time─┐
│            72 │
│           112 │
│           126 │
│           145 │
│           104 │
│           242 │
│           313 │
│           168 │
│           108 │
└───────────────┘
```

Запрос:

``` sql
SELECT quantileTiming(response_time) FROM t
```

Результат:

``` text
┌─quantileTiming(response_time)─┐
│                           126 │
└───────────────────────────────┘
```

**Смотрите также**

-   [median](/ru/sql-reference/aggregate-functions/reference/median)
-   [quantiles](/ru/sql-reference/aggregate-functions/reference/quantiles)
