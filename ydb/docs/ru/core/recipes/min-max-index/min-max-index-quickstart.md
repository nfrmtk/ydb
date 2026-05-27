# min_max индекс — быстрый старт

Ниже минимальный пример: колоночная таблица с первичным ключом и локальным индексом `min_max` по колонке, которая часто используется в условиях фильтрации.

```yql
CREATE TABLE `/Root/events` (
    id Uint64 NOT NULL,
    event_time Timestamp NOT NULL,
    source Utf8,
    payload String,
    PRIMARY KEY (id),
    INDEX idx_event_time LOCAL USING min_max ON (event_time)
)
WITH (
    STORE = COLUMN
);
```

## Расширение примера: индекс по строковой колонке

К той же таблице можно добавить `min_max` индекс по строковой колонке. Это полезно, если данные загружаются пачками от ограниченного числа источников — значения `source` будут коррелировать с порядком хранения:

```yql
ALTER TABLE `/Root/events`
  ADD INDEX idx_source LOCAL USING min_max ON (source);
```

## Запросы и эффект

После загрузки данных запросы с условиями по проиндексированным колонкам могут читать меньше данных: при обходе хранилища `min_max` индекс пропускает фрагменты, в которых запрошенное значение или диапазон выходят за пределы `[min, max]` фрагмента.

Пример данных:

```yql
INSERT INTO `/Root/events` (id, event_time, source, payload) VALUES
    (1, Timestamp("2026-01-01T10:00:00Z"), "service-a", "{}"),
    (2, Timestamp("2026-01-01T11:00:00Z"), "service-b", "{}"),
    (3, Timestamp("2026-01-02T09:00:00Z"), "service-a", "{}");
```

Запрос по диапазону временно́й метки — движок пропустит фрагменты вне запрошенного интервала `event_time`:

```yql
SELECT id, source
FROM `/Root/events`
WHERE event_time BETWEEN Timestamp("2026-01-01T00:00:00Z")
                     AND Timestamp("2026-01-01T23:59:59Z");
```

Точечный поиск по `source` — фрагменты, где `[min, max]` не пересекается со строкой `"service-a"`, будут пропущены:

```yql
SELECT id, event_time
FROM `/Root/events`
WHERE source = "service-a";
```

Дополнительные материалы:

* подробности и ограничения — в статье [min_max индекс](../../dev/min-max-index.md);
* полный синтаксис — в [ALTER TABLE ADD INDEX](../../yql/reference/syntax/alter_table/indexes.md).
