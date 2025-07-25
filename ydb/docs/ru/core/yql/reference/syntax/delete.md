
# DELETE FROM

{% if oss == true and backend_name == "YDB" %}

{% note warning %}

{% include [OLAP_not_allow_text](../../../_includes/not_allow_for_olap_text.md) %}

Вместо `DELETE FROM` для удаления данных из колоночных таблиц можно воспользоваться механизмом удаления строк по времени — [TTL](../../../concepts/ttl.md). TTL можно задать при [создании](create_table/index.md) таблицы через `CREATE TABLE` или [измененить позже](alter_table/index.md) через `ALTER TABLE`.

{% endnote %}

{% endif %}

Удаляет строки из строковой таблицы, подходящие под условия, заданные в `WHERE`.{% if feature_mapreduce %} Таблица ищется по имени в базе данных, заданной оператором [USE](use.md).{% endif %}

## Пример

```yql
DELETE FROM my_table
WHERE Key1 == 1 AND Key2 >= "One";
```

## DELETE FROM ... ON {#delete-on}

Используется для удаления данных на основе результатов подзапроса. Набор колонок, возвращаемых подзапросом, должен быть подмножеством колонок обновляемой таблицы, и в составе возвращаемых колонок обязательно должны присутствовать все колонки первичного ключа таблицы. Типы данных возвращаемых подзапросом колонок должны совпадать с типами данных соответствующих колонок таблицы.

Для поиска удаляемых из таблицы записей используется значение первичного ключа. Присутствие других (неключевых) колонок таблицы в составе выходных колонок подзапроса не влияет на результаты операции удаления.


### Пример

```yql
$to_delete = (
    SELECT Key, SubKey FROM my_table WHERE Value = "ToDelete" LIMIT 100
);

DELETE FROM my_table ON
SELECT * FROM $to_delete;
```

{% if feature_batch_operations %}

## См. также

* [BATCH DELETE](batch-delete.md)

{% endif %}
