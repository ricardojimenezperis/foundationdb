# resolver1

## Evaluation campaign versions

| Campaign id | Implementation | Commit |
|---|---|---|
| `btree-v1` | `TempConflictBTree` (`tempconflictbtree.h`, `tempconflictbtree.cpp`), with `BNodeStore`, `KeyStore` and `PagePool` | `1ec5d3d4d8907e6883568a168bfaab5d1fcca044` |

`btree-v1` names the tree at that commit for the evaluation campaign, whose
candidates are `fdb`, `v107`, `v108` and `btree-v1`. It is not a tag, and
classes and files keep their names. At that commit the full test suite
passes, also under UBSan.

## Author

Ricardo Jimenez-Peris

- Website: <https://ricardojimenezperis.com>
- GitHub: <https://github.com/ricardojimenezperis>
