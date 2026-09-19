#!/usr/bin/env sh
#
# both-accelerators.sh — run one target under TCG and under KVM, and report
# where they disagree (#516).
#
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# Uso:
#   ./scripts/both-accelerators.sh x86-64 [--entry N] [secondi]
#   ./scripts/both-accelerators.sh i386   [secondi] [argomenti per smoke-ush.sh]
#
# I secondi sono il budget di UNA run, non delle due.
#
# I log finiscono in ~/uros-tests/both-<target>-<accel>.log e restano.
#
# ── Perche' esiste ────────────────────────────────────────────────────
#
# 🔑 Ogni risultato x86-64 di questo porto e' stato preso sotto TCG, e ogni
# risultato i386 sotto KVM.  Nessuno dei due target e' mai stato misurato sotto
# entrambi, e ciascuno ha visto solo quello che l'altro non vede mai.
#
# Le divergenze note finora sono TRE, e non hanno tutte la stessa forma:
#
#   #477  TCG accetta un `iretq' con SS.RPL != CS.RPL  ⇒ nasconde un difetto
#   #432  TCG accetta la store di un processore alla propria pagina APIC
#         come se fosse un messaggio                    ⇒ nasconde un difetto
#   #515  TCG NON alza #XF per un'eccezione SIMD non mascherata
#                                                       ⇒ INVENTA un difetto
#
# 🔴 Le prime due sono l'emulatore permissivo: fa passare cio' che l'hardware
# rifiuterebbe, e il kernel sembra sano.  La terza e' l'opposto: non alza cio'
# che l'hardware alza, e un kernel corretto sembra rotto.  La seconda forma e'
# peggio per l'abitudine, perche' insegna a ignorare un braccio rosso.
#
# ── Cosa NON fa ───────────────────────────────────────────────────────
#
# 🔴 Non sceglie un vincitore.  Una divergenza e' un reperto, e risolverla
# prendendo l'acceleratore che passa vuol dire buttare via l'unica cosa che
# questo confronto produce.  Lo script stampa entrambe le versioni e si ferma.
# ⚠️ E nessuno dei due e' il ferro: in #477 e #432 aveva ragione KVM, il che e'
# prova su QUEI due casi e non una classifica.  Solo il bare metal decide.
#
# ⚠️ KVM non sostituisce TCG.  Su un difetto raro o di timing SMP accelerare
# CAMBIA l'esperimento, e quelle cacce vogliono TCG.  Questo aggiunge una run,
# non ne toglie una.

set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
OUT=${UROS_TESTS_DIR:-$HOME/uros-tests}
mkdir -p "$OUT"

TARGET=${1:-}
case "$TARGET" in
x86-64|i386)	shift ;;
*)		echo "uso: $0 x86-64|i386 [argomenti]" >&2; exit 2 ;;
esac

# Un backstop, e SOLO un backstop.
#
# 🔴 I timeout veri stanno dentro l'harness che aspetta: smoke-ush.exp sa se
# sta aspettando un banner, un prompt o un comando, e ha un budget per
# ciascuno.  Questo qui non sa niente -- esiste per il caso in cui sia
# `expect' stesso a piantarsi -- quindi deve stare COMODAMENTE SOPRA e mai
# accanto ai suoi.
#
# ⚠️ La prima versione lo metteva a 1200, cioe' appena sopra il budget di boot
# che avevo alzato a 900: un secondo giudice che non ha visto niente e taglia
# per primo.  Se fosse scattato avrebbe troncato la run a meta' dei comandi e
# prodotto proprio il confronto-fra-troncati contro cui questo script stampa
# un avviso.  Misurato dopo: il boot costa 19 s sotto TCG e 22 sotto KVM, quindi
# i 900 non servivano e il problema era la scelta del numero, non il numero.
BUDGET=1800
case "${1:-}" in
[0-9]*)	BUDGET=$1
	[ "$TARGET" = i386 ] && shift ;;
esac

# ── Le due run ────────────────────────────────────────────────────────
#
# 🔴 SENZA `set -e', e non e' una svista: una run che FALLISCE e' il dato, non
# un errore dello script.  Interrompersi al primo rosso vorrebbe dire non
# arrivare mai a confrontare i due, che e' esattamente il caso in cui il
# confronto serve di piu'.
#
# ⚠️ In sequenza e mai in parallelo: una QEMU per volta.  Due macchine che si
# contendono la CPU dell'host cambierebbero il timing di entrambe, e la lenta
# delle due e' quella che gia' fatica a stare nel budget.
# 🔴 SU i386 SI CONFRONTA LA SMOKE, non un boot nudo, e la ragione e' che UNA
# RUN CHE NON FINISCE NON SI PUO' CONFRONTARE.
#
# run-x86_64.sh sa quando la sua run e' finita: il kernel annuncia un
# terminatore e lo script lo aspetta.  run-qemu.sh no -- boota fino al prompt
# di ush e resta li', quindi ogni run i386 nuda brucia il budget e viene
# uccisa, sotto entrambi gli acceleratori.  Provato: 900 s per braccio, TCG 124
# e KVM 124, e i due "concordavano" su un insieme TRONCATO.
#
# La smoke e' la sola run i386 che finisce da sola, perche' e' l'unica che
# DIGITA `shutdown'.  Quindi e' anche la sola che due acceleratori possano
# confrontare onestamente.
TCG_LOG=$OUT/both-$TARGET-tcg.log
KVM_LOG=$OUT/both-$TARGET-kvm.log

echo "=== both-accelerators: $TARGET, TCG first ==="
case "$TARGET" in
x86-64)
	UROS_X86_64_LOG=$TCG_LOG "$SCRIPT_DIR/run-x86_64.sh" "$@"
	TCG_RC=$?
	;;
i386)
	timeout "$BUDGET" "$SCRIPT_DIR/smoke-ush.sh" --tcg "$@" \
		</dev/null >"$TCG_LOG" 2>&1
	TCG_RC=$?
	;;
esac

echo "=== both-accelerators: $TARGET, now KVM ==="
case "$TARGET" in
x86-64)
	UROS_X86_64_LOG=$KVM_LOG "$SCRIPT_DIR/run-x86_64.sh" "$@" -enable-kvm
	KVM_RC=$?
	;;
i386)
	timeout "$BUDGET" "$SCRIPT_DIR/smoke-ush.sh" "$@" \
		</dev/null >"$KVM_LOG" 2>&1
	KVM_RC=$?
	;;
esac

# ── Cosa si confronta, e cosa si normalizza ───────────────────────────
#
# Si confrontano le righe che AFFERMANO UN ESITO, non i log interi: due boot
# della stessa immagine differiscono in indirizzi, cicli e microsecondi senza
# che nessuna di quelle differenze sia una divergenza fra acceleratori.
#
# 🔴 E si normalizza il MENO possibile, perche' la normalizzazione e' il punto
# in cui un confronto puo' mentire.  Vanno via solo due cose, entrambe volatili
# per costruzione:
#
#   - gli indirizzi esadecimali, che cambiano fra due boot qualunque;
#   - i numeri che portano un'unita' di tempo (us/op, ns/op, cicli, ms),
#     perche' KVM e TCG differiscono li' di due ordini di grandezza ED E'
#     ATTESO -- quella non e' la divergenza che questo script cerca.
#
# ⚠️ Gli interi NUDI restano.  `code 16' e `code 19' differiscono di un numero
# e sono due guasti diversi: cancellarli renderebbe lo strumento cieco proprio
# alla forma di divergenza che #515 ha trovato.
findings() {
	grep -aE 'PASS|FAIL|WRONG|SKIPPED|panic|Assertion failed' "$1" 2>/dev/null \
	| sed -e 's/0x[0-9a-fA-F][0-9a-fA-F]*/0xX/g' \
	      -e 's/[0-9][0-9]*\( *\)\(us\/op\|ns\/op\|cycles\|ms\|us\)/N\1\2/g' \
	      -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' \
	| sort -u
}

findings "$TCG_LOG" > "$OUT/.both-tcg.$$"
findings "$KVM_LOG" > "$OUT/.both-kvm.$$"

ONLY_TCG=$(comm -23 "$OUT/.both-tcg.$$" "$OUT/.both-kvm.$$")
ONLY_KVM=$(comm -13 "$OUT/.both-tcg.$$" "$OUT/.both-kvm.$$")
rm -f "$OUT/.both-tcg.$$" "$OUT/.both-kvm.$$"

echo
echo "=== both-accelerators: $TARGET ==="
echo "  TCG: exit $TCG_RC   log $TCG_LOG"
echo "  KVM: exit $KVM_RC   log $KVM_LOG"

# ⚠️ 124 e' il codice con cui `timeout' uccide: quella run non e' finita, e i
# suoi reperti sono quelli che ha fatto in tempo a stampare.  Detto qui perche'
# un confronto fra una run intera e una troncata trova divergenze che sono solo
# il troncamento.
for _rc in "TCG:$TCG_RC" "KVM:$KVM_RC"; do
	case "$_rc" in
	*:124)	echo "  ⚠️ ${_rc%%:*} hit the ${BUDGET}s budget and was killed —"
		echo "     its findings are only what it printed before that, so a"
		echo "     disagreement below may be the truncation and not the"
		echo "     accelerator" ;;
	esac
done

if [ -z "$ONLY_TCG" ] && [ -z "$ONLY_KVM" ]; then
	# ⚠️ "Agree" e non "pass": i due possono concordare su un fallimento, e
	# quello e' un difetto del kernel che entrambi vedono -- il caso piu'
	# facile da leggere e il piu' facile da confondere con un verde.
	echo "  the two accelerators AGREE on every outcome line"
	echo "  (which says nothing about whether those outcomes were good)"
	exit 0
fi

echo
echo "  🔴 THE TWO ACCELERATORS DISAGREE — reported, not resolved"
[ -n "$ONLY_TCG" ] && { echo "  only under TCG:"; printf '%s\n' "$ONLY_TCG" | sed 's/^/    /'; }
[ -n "$ONLY_KVM" ] && { echo "  only under KVM:"; printf '%s\n' "$ONLY_KVM" | sed 's/^/    /'; }
echo
echo "  Neither side is the answer.  Both are emulators of a machine that is"
echo "  not here, and only bare metal settles which one was right."
exit 3
