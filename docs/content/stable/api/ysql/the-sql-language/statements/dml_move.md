---
title: MOVE statement [YSQL]
headerTitle: MOVE
linkTitle: MOVE
description: Use the MOVE statement to change the position of the current row in a cursor.
menu:
  stable:
    identifier: dml_move
    parent: statements
type: docs
---

{{< warning title="YSQL currently supports only fetching rows from a cursor consecutively in the forward direction." >}}
See the subsection [Beware Issue #6514](../../../cursors/#beware-issue-6514) in the generic section [Cursors](../../../cursors/).
{{< /warning >}}

## Synopsis

Use the `MOVE` statement to change the position of the current row in a _[cursor](../../../cursors/)_. The `MOVE` statement is used jointly with the [`DECLARE`](../dml_declare), [`FETCH`](../dml_fetch), and [`CLOSE`](../dml_close) statements.

## Syntax

<ul class="nav nav-tabs nav-tabs-yb">
  <li >
    <a href="#grammar" class="nav-link" id="grammar-tab" data-toggle="tab" role="tab" aria-controls="grammar" aria-selected="true">
      <img src="/icons/file-lines.svg" alt="Grammar Icon">
      Grammar
    </a>
  </li>
  <li>
    <a href="#diagram" class="nav-link active" id="diagram-tab" data-toggle="tab" role="tab" aria-controls="diagram" aria-selected="false">
      <img src="/icons/diagram.svg" alt="Diagram Icon">
      Diagram
    </a>
  </li>
</ul>

<div class="tab-content">
  <div id="grammar" class="tab-pane fade" role="tabpanel" aria-labelledby="grammar-tab">
  {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/move,move_to_one_row,move_over_many_rows.grammar.md" %}}
  </div>
  <div id="diagram" class="tab-pane fade show active" role="tabpanel" aria-labelledby="diagram-tab">
  {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/move,move_to_one_row,move_over_many_rows.diagram.md" %}}
  </div>
</div>

## Semantics

`MOVE` changes the position of the current row in a _cursor_.

A _cursor_ represents the current position in its result set. After declaring a cursor but before the first `FETCH` or `MOVE` execution, the current position is immediately _before_ the first row.

- The `MOVE 0` and `MOVE FORWARD 0` variants leave the current position unchanged. They therefore has no practical value

- The bare `MOVE` variant, the `MOVE NEXT` variant, the bare `MOVE FORWARD` variant, and the `MOVE FORWARD 1` variant all update the current position to one row after where if was before invoking the statement. If before executing one of these `MOVE` variants, the current position is the last row in the result set, then the current position us set to _after_ the last row. There are no flavors of _after the last row_. It's a uniquely defined state so that following any number of invocations of `MOVE NEXT` in this state, `MOVE PRIOR` will then fetch the last row in the result set (and update the current position to that last row.)

- The `MOVE PRIOR` variant, the bare `MOVE BACKWARD` variant, and the `MOVE BACKWARD 1` variant all update the current position to one row before where if was before invoking the statement. If before executing one of these `MOVE` variants, the current position is the first row in the result set, then the current position us set to _before_ the first row. There are no flavors of _before the first row_. It's a uniquely defined state so that following any number of invocations of `MOVE PRIOR` in this state, `MOVE NEXT` will update the current position to the first row.

- `MOVE ALL` (and `MOVE FORWARD ALL`) move over all the rows from the row immediately after the current position through the last row, and the cursor position is left _after_ the last row. Of course, if when `MOVE ALL` (or `MOVE FORWARD ALL`) is invoked, the current position is already _after_ the last row, then the current position is left _after_ the last row.

- `MOVE BACKWARD ALL` moves over all the rows from the row immediately before the current position through the first row, and the cursor position is left _before_ the first row. Of course, if when `MOVE BACKWARD ALL` is invoked, the current position is already _before_ the first row, then the current position is left _before_ the first row.

- The `MOVE :n` (and `MOVE FORWARD :n`) variants move over exactly _:n_ rows forwards from and including the row after the current position when this many rows are available and otherwise over just as many as it can analogously to how `MOVE FORWARD ALL` behaves.

- The `MOVE BACKWARD :n` variant moves over exactly _:n_ rows backwards from and including the row before the current position when this many rows are available and otherwise just as many as it can analogously to how `MOVE BACKWARD ALL` behaves.

- The `MOVE ABSOLUTE :n` variant moves to the single row at exactly the indicated absolute position. The `MOVE RELATIVE :n` variant moves to the single row at exactly the indicated relative position (_:n_ can be negative) to the current row. For both `MOVE ABSOLUTE :n` and `MOVE RELATIVE :n`, the requested row might lie before the first row or after the last row. The outcome here is the same as it is when executing other `MOVE` variants that cause the current position to fall outside the range from the first through the last row in the cursor's result set. Notice that _:n_ can be negative for both the `ABSOLUTE` and the `RELATIVE` variants.

- Each of the `MOVE FIRST` and `MOVE LAST` variants moves, respectively, to the first row or the last row. The meanings are therefore insensitive to the current cursor position, and each can be repeated time and again and will always have the same effect. 

Notice that the three variants ,`MOVE FORWARD 0`, `MOVE BACKWARD 0`, and `MOVE RELATIVE 0`, all mean the same as each other.

### *name*

A _cursor_ is identified only by an unqualified name and is visible only in the session that declares it. This determines the uniqueness scope for its name. (The name of a  _cursor_ is like that of a _prepared statement_ in this respect.)

## Simple example


```plpgsql
bla
```

This is the result...

```output
bla
```

## See also

- [`DECLARE`](../dml_declare)
- [`FETCH`](../dml_fetch)
- [`CLOSE`](../dml_close)