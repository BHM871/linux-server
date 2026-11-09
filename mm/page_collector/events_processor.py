#!/usr/bin/env python3

import argparse
import csv
import sqlite3
import time
from pathlib import Path

BATCH_SIZE = 100_000


def create_database(db):
    # Reinicia as tabelas para não misturar execuções anteriores.
    db.executescript("""
        DROP TABLE IF EXISTS page_counts;
        DROP TABLE IF EXISTS process_stats;

        CREATE TABLE process_stats (
            pid TEXT PRIMARY KEY,
            comm TEXT NOT NULL,
            total_events INTEGER NOT NULL DEFAULT 0,
            total_h INTEGER NOT NULL DEFAULT 0,
            total_f INTEGER NOT NULL DEFAULT 0
        );

        CREATE TABLE page_counts (
            pid TEXT NOT NULL,
            event_type TEXT NOT NULL,
            page_id TEXT NOT NULL,
            total INTEGER NOT NULL DEFAULT 0,
            PRIMARY KEY (pid, event_type, page_id)
        ) WITHOUT ROWID;
    """)


def process_csv(csv_path, db):
    processed = 0
    ignored = 0

    with open(
        csv_path, "r", newline="", encoding="utf-8-sig"
    ) as file:
        reader = csv.DictReader(file)

        required = {"timestamp_ns", "pid", "tgid", "page_id", "event_type", "comm"}
        if not reader.fieldnames or not required.issubset(reader.fieldnames):
            raise ValueError(
                f"CSV precisa conter as colunas: {sorted(required)}"
            )

        batch_stats = {}
        batch_pages = {}

        def flush():
            if not batch_stats and not batch_pages:
                return

            with db:
                db.executemany("""
                    INSERT INTO process_stats
                        (pid, comm, total_events, total_h, total_f)
                    VALUES (?, ?, ?, ?, ?)
                    ON CONFLICT(pid) DO UPDATE SET
                        total_events =
                            total_events + excluded.total_events,
                        total_h = total_h + excluded.total_h,
                        total_f = total_f + excluded.total_f
                """, [
                    (pid, comm, *counts)
                    for (pid, comm), counts in batch_stats.items()
                ])

                db.executemany("""
                    INSERT INTO page_counts
                        (pid, event_type, page_id, total)
                    VALUES (?, ?, ?, ?)
                    ON CONFLICT(pid, event_type, page_id)
                    DO UPDATE SET total = total + excluded.total
                """, [
                    (pid, event_type, page_id, count)
                    for (pid, event_type, page_id), count in batch_pages.items()
                ])

            batch_stats.clear()
            batch_pages.clear()

        for row in reader:
            try:
                pid = row["pid"].strip()
                page_id = row["page_id"].strip()
                event_type = row["event_type"].strip().upper()
                comm = row["comm"].strip()

                if not pid or not page_id or not comm:
                    raise ValueError("PID, TGID, página ou command vazio")

                if event_type not in {"H", "F"}:
                    raise ValueError("Tipo de evento inválido")

                int(pid)
                int(page_id)

            except (ValueError, TypeError, AttributeError):
                ignored += 1
                continue

            key = (pid, comm)
            if key not in batch_stats:
                batch_stats[key] = [0, 0, 0]

            counts = batch_stats[key]
            counts[0] += 1

            if event_type == "H":
                counts[1] += 1
            else:
                counts[2] += 1

            key = (pid, event_type, page_id)
            batch_pages[key] = batch_pages.get(key, 0) + 1

            processed += 1

            if processed % BATCH_SIZE == 0:
                flush()
                print(f"Processados: {processed:,} eventos")

        flush()

    return processed, ignored


def write_csv(output_path, headers, rows):
    """Escreve as linhas progressivamente, sem guardar a tabela na RAM."""
    with open(
        output_path, "w", newline="", encoding="utf-8-sig"
    ) as file:
        writer = csv.writer(file)
        writer.writerow(headers)

        for row in rows:
            writer.writerow(row)


def export_csvs(db, output_dir):
    output_dir.mkdir(parents=True, exist_ok=True)

    # 1. Resumo por processo
    def process_summary():
        rows = db.execute("""
            SELECT pid, comm, total_events, total_h, total_f
            FROM process_stats
            ORDER BY CAST(pid AS INTEGER)
        """)

        for pid, comm, total, h_count, f_count in rows:
            h_rate = h_count / total if total else 0
            f_rate = f_count / total if total else 0

            yield (
                pid, comm, total, h_count, f_count,
                f"{h_rate:.8f}", f"{f_rate:.8f}"
            )

    write_csv(
        output_dir / "processos.csv",
        [
            "processo",
            "name",
            "total_eventos",
            "total_H",
            "total_F",
            "taxa_H",
            "taxa_F",
        ],
        process_summary(),
    )

    # 2. Faltas por página virtual: somente eventos H
    def virtual_faults():
        return db.execute("""
            SELECT pid, page_id, total
            FROM page_counts
            WHERE event_type = 'H'
            ORDER BY CAST(pid AS INTEGER), total DESC
        """)

    write_csv(
        output_dir / "faltas_virtuais.csv",
        ["processo", "pagina_virtual", "total_faltas"],
        virtual_faults(),
    )

    # 3. Resumo de acessos físicos por processo
    def physical_summary():
        processes = db.execute("""
            SELECT pid, total_f
            FROM process_stats
            ORDER BY CAST(pid AS INTEGER)
        """)

        for pid, total_f in processes:
            distinct_pages = db.execute("""
                SELECT COUNT(*)
                FROM page_counts
                WHERE pid = ? AND event_type = 'F'
            """, (pid,)).fetchone()[0]

            top10 = db.execute("""
                SELECT COALESCE(SUM(total), 0)
                FROM (
                    SELECT total
                    FROM page_counts
                    WHERE pid = ? AND event_type = 'F'
                    ORDER BY total DESC
                    LIMIT 10
                )
            """, (pid,)).fetchone()[0]

            concentration = top10 / total_f if total_f else 0

            yield (
                pid,
                total_f,
                distinct_pages,
                f"{concentration:.8f}",
            )

    write_csv(
        output_dir / "resumo_fisico.csv",
        [
            "processo",
            "total_acessos_F",
            "paginas_fisicas_distintas",
            "concentracao_top10",
        ],
        physical_summary(),
    )


def main():
    parser = argparse.ArgumentParser(
        description="Agrega eventos H/F e gera tabelas CSV."
    )
    parser.add_argument("input", type=Path, help="CSV de entrada")
    parser.add_argument(
        "-o", "--output-dir",
        type=Path,
        default=Path("resultados"),
        help="Diretório dos CSVs (padrão: resultados)",
    )
    parser.add_argument(
        "--database",
        type=Path,
        default=Path("event_aggregation.sqlite"),
        help="Banco temporário SQLite",
    )
    args = parser.parse_args()

    if not args.input.is_file():
        parser.error(f"CSV não encontrado: {args.input}")

    if args.input.resolve() == args.database.resolve():
        parser.error("O CSV de entrada e o banco precisam ser diferentes")

    start = time.perf_counter()

    print("[+] Preparando banco SQLite...")
    with sqlite3.connect(args.database) as db:
        # Ajuste útil para processamento em disco.
        db.execute("PRAGMA journal_mode = WAL")
        db.execute("PRAGMA synchronous = NORMAL")
        create_database(db)

        print("[+] Processando eventos...")
        processed, ignored = process_csv(args.input, db)

        print("[+] Exportando tabelas CSV...")
        export_csvs(db, args.output_dir)

    elapsed = time.perf_counter() - start

    print("\n[✓] Processamento concluído")
    print(f"Eventos processados: {processed:,}")
    print(f"Linhas ignoradas:    {ignored:,}")
    print(f"Diretório de saída:  {args.output_dir}")
    print(f"Tempo total:         {elapsed:.2f} s")


if __name__ == "__main__":
    main()

