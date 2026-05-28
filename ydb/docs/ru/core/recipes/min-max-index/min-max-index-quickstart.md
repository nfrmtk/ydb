# min_max индекс — быстрый старт

Ниже минимальный пример: колоночная таблица с первичным ключом и локальным индексом `min_max` по колонке `event_time`;

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

## Запросы и эффект

После загрузки данных запросы с условиями по проиндексированным колонкам могут читать меньше данных: при обходе хранилища `min_max` индекс пропускает фрагменты, в которых запрошенное значение или диапазон выходят за пределы `[min, max]` фрагмента.

Пример данных:

```yql
$rows = ListMap(
    ListFromRange(1, 10000001),
    ($i) -> (<|
        id: CAST($i AS Int64),
        event_time: Timestamp("2026-01-01T00:00:01Z") + DateTime::FromSeconds($i),
        source: CAST("event_" || CAST($i AS String) AS Bytes),
        payload: "foo"
    |>)
);

UPSERT INTO `/Root/events`
SELECT * FROM AS_TABLE($rows);
```

Запрос по диапазону временно́й метки — движок пропустит фрагменты вне запрошенного интервала `event_time`(а таких скорее всего большинство):

```yql
SELECT id, source
FROM `/Root/events`
WHERE event_time BETWEEN Timestamp("2026-01-01T00:00:01Z")
                     AND Timestamp("2026-01-01T00:00:02Z");
```

Дополнительные материалы:

* подробности и ограничения — в статье [min_max индекс](../../dev/min-max-index.md);
* полный синтаксис — в [ALTER TABLE ADD INDEX](../../yql/reference/syntax/alter_table/indexes.md).
