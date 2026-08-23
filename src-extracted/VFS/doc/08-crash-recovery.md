# Crash Recovery

## Journal-Based Recovery

InvariantFS uses an append-only journal in the Metadata Zone for crash-safe metadata updates.

### Journal Entry Types

| Type byte | Entry | Description |
|-----------|-------|-------------|
| `0x01` | `MAP(inode, lba, pba, len)` | Assign logical-to-physical mapping |
| `0x02` | `UNMAP(inode, lba)` | Release mapping |
| `0x03` | `SWEEP_COMMIT(inode, old_raw_blocks, new_shadow_blocks)` | Sweep atomic commit |
| `0x04` | `DELTA(inode, tag_path, value)` | Tag edit delta |
| `0x05` | `META(inode, field, value)` | Metadata change (perms, times) |
| `0xFF` | `CHECKPOINT(snapshot_offset)` | Full L2P snapshot follows |

### Mount Recovery

```
On mount:
  1. Read Superblock → check state
     └─ Clean (0xCA) → normal mount
     └─ Dirty (0xDA) → replay journal
     └─ Recovery (0xRE) → replay journal + verify

  2. Replay journal from last CHECKPOINT:
     └─ Rebuild in-memory L2P table
     └─ Rebuild in-memory Bitmap
     └─ Rebuild in-memory Primary Index
     └─ Rebuild in-memory Tag Index

  3. Check for incomplete SWEEP operations:
     └─ If SWEEP_COMMIT found → keep Shadow data, free RAW
     └─ If SWEEP_BEGIN without COMMIT → invalidate Shadow, keep RAW

  4. Verify invariant: sample check random swept files
     └─ Decompress → compare BLAKE3 hash vs stored hash

  5. Set state → Clean (0xCA)
  6. Flush superblock
```

### Graceful Shutdown

```
On unmount:
  1. Flush L2P journal
  2. Flush Bitmap
  3. Create CHECKPOINT (compact L2P + Bitmap snapshot)
  4. Set state → Clean (0xCA)
  5. Flush superblock
```

## Consistency Guarantees

| Scenario | Outcome |
|----------|---------|
| Crash during RAW write | Lost write (data in buffer not flushed). File marked incomplete in L2P. |
| Crash during journal flush | Journal replay picks up from last CHECKPOINT. Max 1 checkpoint interval of data loss. |
| Crash during Sweep | SWEEP_BEGIN without COMMIT → RAW data preserved, Shadow data invalidated. |
| Crash during CHECKPOINT | Previous CHECKPOINT still valid. |
| Bit error in Shadow data | BLAKE3 mismatch on read → return EIO, schedule re-sweep from RAW backup. |

## Superblock State Transitions

```
Clean ──mount──→ Dirty ──recovery──→ Recovery ──replay──→ Clean
  ↑                                                    │
  └─────────────────unmount────────────────────────────┘
```

## fsck / repair (2025-08): invf-fsck

`invf-fsck <image> [-f|--fix]` проверяет и чинит том:

- **CRC inode-записей**: битые записи отмечены (bad_recs); vol_open и
  invf-ls теперь ПЕРЕЖИВАЮТ битую запись (skip по rec_len+4), так что
  файлы ПОСЛЕ неё остаются доступными (раньше зона обрезалась).
- **AST↔L2P**: каждый живой сегмент обязан иметь L2P-запись
  (l2p_miss = данные потеряны — формат хранит в AST только индекс
  сегмента, физический блок живёт в журнале, так что L2P из AST
  не пересобирается).
- **Битмап-ремонт**: used = metadata-зона + pba из живого L2P;
  осиротевшие блоки (заняты, без ссылок) освобождаются, потерянные
  (есть ссылка, но свободны) восстанавливаются.
- **Журнал-компакт**: при -f журнал переписывается только из живых
  L2P-записей (мусор от мёртвых inode удаляется), суперблок → CLEAN.
