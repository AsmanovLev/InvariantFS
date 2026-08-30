# Security & Cross-Platform Permissions

> **Статус:** POSIX-половина реализована иначе — uid/gid/mode/type/times
> живут в INO2 meta-ext записи (format v2, VOLF_META2; «Primary Index»
> из этого документа не строился, см. 02-on-disk-format.md). Проверка прав
> при доступе (enforcement) и Windows-половина (SID-маппинг, NTFS ACL)
> **не реализованы**; Windows-порт EOL (09-windows-port.md), так что
> разделы про SID ниже — исторический контекст.

## POSIX + Windows Гибридная модель

Первичный индекс хранит расширенную структуру прав, совместимую с обеими платформами:

```
struct inode_meta {
    uint32_t uid;              // POSIX UID (Linux)
    uint32_t gid;              // POSIX GID (Linux)
    uint16_t mode;             // POSIX rwxrwxrwx + sticky/setuid/setgid
    uint32_t win_attrs;        // Windows атрибуты (Hidden, System, Archive, ReadOnly)
    // Опционально: расширенные атрибуты (xattr)
}
```

### Linux

Ядро напрямую читает `uid`, `gid`, `mode`. `win_attrs` игнорируется (или доступно через `ioctl`).

### Windows (через WinFsp) — Маппинг SID

**Проблема:** Windows идентифицирует пользователей через SID (Security Identifier), например `S-1-5-21-...`, а не через 32-битный UID. Если просто хранить `uid=1000`, Windows покажет владельца как "Неизвестный пользователь".

**Решение — два режима:**

#### 1. Простой режим (по умолчанию)

Весь диск монтируется от имени одного пользователя (как сетевой диск). POSIX mode транслируется в NTFS ACL для групп "Everyone" / "Authenticated Users":

| POSIX mode | NTFS ACL |
|-----------|----------|
| `rwxr-xr-x` | Everyone: Read&Execute, Owner: Full Control |
| `rw-------` | Owner: Full Control, Everyone: None |
| `rwx------` | Owner: Full Control только |
| `rw-rw-rw-` | Everyone: Read&Write |

Никакого маппинга UID → SID не требуется. Все пользователи Windows видят одинаковые права.

#### 2. Расширенный режим (SID в xattr)

Если требуется точное соблюдение прав (multiuser):

```
xattr блок (Text Zone):
─────────────────────────
{
  "security.selinux": "...",
  "security.capability": "...",
  "system.ntfs_acl": "<binary blob с SID>",
  "user.comment": "..."
}

Плюс in-memory кэш SID <-> UID:
  struct { uint32_t uid; wchar_t* sid_string; } mapping_cache;
```

- SID хранится в xattr, загружается только при явном вызове `GetSecurity` WinFsp
- In-memory кэш маппинга (10-100K записей) для быстрого преобразования
- Если SID не найден в кэше — Fallback к простому режиму (Everyone)

## Совместимость при монтировании

```
Linux:  mount -t invarifs /dev/sda2 /mnt
        → uid/gid/mode читаются напрямую из Primary Index
        → ACL не требуется (ядро само раздаёт права)
        → win_attrs и xattr игнорируются

Windows: WinFsp монтирует как диск X:
         → Простой режим: POSIX mode → Everyone ACL
         → Расширенный режим: SID из xattr + in-memory кэш
         → win_attrs → FILE_ATTRIBUTE_HIDDEN и т.д.
```

## Наследование прав

Новые файлы наследуют права родительской директории (как в POSIX).
При создании через WinFsp — права от процесса, транслированные в POSIX mode через маппинг.
