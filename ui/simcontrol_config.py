#!/usr/bin/env python3
"""SimControl config panel — reads/writes simcontrol.conf and applies live
via /dev/shm/simcontrol_ipc."""

from __future__ import annotations

import ctypes
import json
import mmap as mmap_mod
import os
import re
import signal
import subprocess
import time
from dataclasses import dataclass, fields
from pathlib import Path

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
from gi.repository import Gdk, GLib, Gio, Gtk, Pango

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
CONF_CANDIDATES = [
    ROOT / "simcontrol.conf",
    Path.cwd() / "simcontrol.conf",
    Path.home() / ".config" / "simcontrol" / "simcontrol.conf",
]
PRESETS_PATH = Path.home() / ".config" / "simcontrol" / "presets.json"  # legacy
PRESETS_DIR = Path.home() / ".config" / "simcontrol" / "presets"
SRC2GID = {
    1: "ams2", 4: "pcars2", 8: "pcars1", 2: "ac-evo", 3: "ac-rally",
    5: "raceroom", 6: "ams1", 7: "rf2",
}

GAME_GROUPS = [
    ("ams2",     "Automobilista 2"),
    ("pcars2",   "Project CARS 2"),
    ("pcars1",   "Project CARS 1"),
    ("ac-evo",   "Assetto Corsa Evo"),
    ("ac-rally", "Assetto Corsa Rally"),
    ("raceroom", "RaceRoom"),
    ("ams1",     "Automobilista 1"),
    ("rf2",      "rFactor 2"),
]

SRC2LABEL = {src: lbl for src, gid in SRC2GID.items()
           for g, lbl in GAME_GROUPS if g == gid}
LEGACY_GAME_MAP = {
    "AMS2/PCARS2": ["ams2", "pcars2"],   # same preset as Stock for both
    "ACEvo":       ["ac-evo"],
    "AC Rally":    ["ac-rally"],
    "Raceroom":    ["raceroom"],
    "RaceRoom":    ["raceroom"],
    "AMS1/rF2":    ["ams1", "rf2"],
}
UI_PREFS_PATH = Path.home() / ".config" / "simcontrol" / "ui.json"
BUILTIN_PRESET = "Stock"

SIMCONTROL_BIN_CANDIDATES = [
    ROOT / "simcontrol",
    Path("/usr/local/bin/simcontrol"),
    Path("/usr/bin/simcontrol"),
]
SIMCONTROL_LOG = (
    Path(os.environ.get("XDG_STATE_HOME", str(Path.home() / ".local" / "state")))
    / "simcontrol"
    / "simcontrol.log"
)

# ---------------------------------------------------------------- i18n ----

LANGS = ("pt", "en")

STR = {
    "win_title": ("SimControl — config", "SimControl — settings"),
    "lang_label": ("Idioma", "Language"),
    "restart_btn": ("Reiniciar SimControl", "Restart SimControl"),
    "restart_tip": (
        "Mata e sobe o processo simcontrol de novo. Usa isso se voltar do "
        "menu do jogo (ex.: AC Evo) travar a direção.",
        "Kills and restarts the simcontrol process. Use it if returning from "
        "a game menu (e.g. AC Evo) leaves steering stuck.",
    ),
    "hdr_geral": ("Geral", "General"),
    "hdr_presets": ("Presets", "Presets"),
    "hdr_steer": ("Direção", "Steering"),
    "hdr_selfsteer": ("Self-steer (contraesterço automático)", "Self-steer (automatic countersteer)"),
    "hdr_pad": ("Analógico", "Gamepad"),
    "hdr_model": ("Modelo / eixos", "Model / axes"),
    "chk_assist_enabled": ("Assistência ligada", "Assist enabled"),
    "chk_passthrough": ("Passthrough (analógico 1:1, sem filtro)", "Passthrough (raw stick 1:1, no filter)"),
    "preset_placeholder": ("Nome do preset", "Preset name"),
    "btn_load": ("Carregar", "Load"),
    "btn_save": ("Salvar", "Save"),
    "btn_delete": ("Apagar", "Delete"),
    "btn_export": ("Exportar…", "Export…"),
    "btn_import": ("Importar…", "Import…"),
    "btn_reset": ("Restaurar valores funcionais", "Restore factory values"),
    "sl_steering_rate": ("Velocidade do filtro", "Filter speed"),
    "sl_rate_increase_with_speed": ("Filtro com a velocidade", "Filter vs speed"),
    "sl_target_slip_deg": ("Slip alvo (graus)", "Target slip (degrees)"),
    "sl_target_slip_scale": ("Escala do slip", "Slip scale"),
    "sl_countersteer_response": ("Resposta de contraesterço", "Countersteer response"),
    "sl_max_dynamic_limit_reduction": ("Limitador dinâmico (0–10)", "Dynamic limiter (0–10)"),
    "sl_self_steer_response": ("Resposta", "Response"),
    "sl_max_self_steer_angle": ("Ângulo máximo", "Max angle"),
    "sl_damping_strength": ("Damping", "Damping"),
    "sl_stick_gamma": ("Gamma", "Gamma"),
    "sl_deadzone": ("Deadzone", "Deadzone"),
    "chk_invert_throttle": ("Inverter acelerador", "Invert throttle"),
    "chk_invert_brake": ("Inverter freio", "Invert brake"),
    "sl_steering_lock_deg": ("Lock assumido", "Assumed lock"),
    "sl_wheelbase_m": ("Entre-eixos", "Wheelbase"),
    "sg_steer_sign": ("Inverter analógico (steer_sign)", "Invert stick (steer_sign)"),
    "sg_lat_sign": ("Inverter self-steer (lat_sign)", "Invert self-steer (lat_sign)"),
    "sg_yaw_sign": ("Inverter yaw", "Invert yaw"),
    "sg_fwd_sign": ("Sinal Z (fwd_sign)", "Z sign (fwd_sign)"),
    "chk_swap_xz": ("Trocar X/Z da telemetria", "Swap telemetry X/Z"),
}

HELP = {
    "assist_enabled": (
        "Liga ou desliga o filtro de direção. Desligado, o analógico vai 1:1 para o volante virtual.",
        "Toggles the steering filter. Off, the stick goes 1:1 to the virtual wheel.",
    ),
    "passthrough": (
        "Ignora a assistência (útil nos menus e para testar se o volante virtual está mapeado).",
        "Bypasses the assist (useful in menus and to check the virtual wheel is mapped).",
    ),
    "steering_rate": (
        "O quão rápido o volante acompanha o analógico. Mais baixo = mais suave e estável, mas com atraso. Mais alto = mais direto.",
        "How fast the wheel follows the stick. Lower = smoother/stabler but laggy. Higher = more direct.",
    ),
    "rate_increase_with_speed": (
        "Muda a velocidade do filtro conforme a velocidade do carro. Negativo = mais lento em alta; positivo = mais rápido.",
        "Scales filter speed with car speed. Negative = slower at high speed; positive = faster.",
    ),
    "target_slip_deg": (
        "Ângulo de slip (graus) que as rodas da frente tentam manter. Mais alto = mais direção / mais raspar de pneu; mais baixo = menos lock na curva.",
        "Slip angle (degrees) the front wheels try to hold. Higher = more steering/tire scrub; lower = less cornering lock.",
    ),
    "target_slip_scale": (
        "Multiplica o slip alvo. 1.00 = o valor em graus acima; 0.95 = um pouco menos de direção.",
        "Multiplies the target slip. 1.00 = the degree value above; 0.95 = slightly less steering.",
    ),
    "countersteer_response": (
        "Quando VOCÊ contraesterça num slide, quanto lock extra o assist libera. Alto = contraesterço mais eficaz, fácil de passar para o outro lado.",
        "When YOU countersteer a slide, how much extra lock the assist grants. High = stronger countersteer, easier to swing across.",
    ),
    "max_dynamic_limit_reduction": (
        "Quanto o assist corta direção para DENTRO quando o carro sobrevira, para não matar a frente. Alto = o volante recua mais no oversteer.",
        "How much the assist cuts steering INTO oversteer to avoid killing the front. High = wheel backs off more on oversteer.",
    ),
    "self_steer_response": (
        "Força que recentraliza o carro sozinha (contraesterço automático). Baixo = mais solto, fácil de sobrevirar. Alto = mais estável.",
        "Force that straightens the car by itself (auto countersteer). Low = looser, easy to spin up. High = stabler.",
    ),
    "max_self_steer_angle": (
        "Teto do self-steer, em graus. 90° deixa recuperar slides grandes; 4–8° deixa o carro mais solto.",
        "Self-steer ceiling in degrees. 90° recovers big slides; 4–8° keeps the car looser.",
    ),
    "damping_strength": (
        "Amortece a rotação (yaw). Em geral deixe parecido com a Resposta. Aumente se o carro oscilar ao voltar ao centro.",
        "Damps rotation (yaw). Usually keep close to Response. Raise it if the car oscillates when centering.",
    ),
    "stick_gamma": (
        "Suaviza o centro do analógico (no AMS2 deixe o gamma em 0 — isso aqui já faz esse trabalho).",
        "Softens stick centering (in AMS2 set in-game gamma to 0 — this already does that job).",
    ),
    "deadzone": (
        "Zona morta do analógico para não mandar input com drift. O mais baixo possível sem o carro virar sozinho parado.",
        "Stick deadzone so stick drift isn't sent. As low as possible without the car steering by itself while parked.",
    ),
    "invert_throttle": (
        "Inverte o eixo do acelerador no volante virtual (se o gatilho direito frear em vez de acelerar).",
        "Inverts the throttle axis on the virtual wheel (if the right trigger brakes instead of accelerating).",
    ),
    "invert_brake": (
        "Inverte o eixo do freio no volante virtual.",
        "Inverts the brake axis on the virtual wheel.",
    ),
    "steering_lock_deg": (
        "Lock assumido das rodas (não o volante). Só para normalizar a matemática. 20° serve na maior parte dos carros.",
        "Assumed road-wheel lock (not the wheel). Only normalizes math. 20° works for most cars.",
    ),
    "wheelbase_m": (
        "Distância entre-eixos do modelo de bicicleta. 2,6 m é um valor médio; F-Vee é um pouco mais curta.",
        "Bicycle-model wheelbase. 2.6 m is average; F-Vee is a bit shorter.",
    ),
    "steer_sign": (
        "Inverte esquerda/direita do analógico. Use −1 se o volante virar ao contrário do stick.",
        "Flips stick left/right. Use −1 if the wheel turns opposite the stick.",
    ),
    "lat_sign": (
        "Inverte o self-steer. Use −1 se o assist empurrar PARA DENTRO do slide em vez de contraesterçar.",
        "Flips self-steer. Use −1 if the assist pushes INTO the slide instead of countersteering.",
    ),
    "yaw_sign": (
        "Inverte o sinal do yaw da telemetria. Só se o damping/self-steer lutar para o lado errado na curva.",
        "Flips telemetry yaw sign. Only if damping/self-steer fights the wrong way in corners.",
    ),
    "fwd_sign": (
        "Sinal do eixo Z local. No AMS2 em geral é −1 (Z para trás). Se o HUD mostrar lim≈1.00 em velocidade, troque este valor.",
        "Local Z-axis sign. On AMS2 usually −1 (Z backwards). If HUD shows lim≈1.00 at speed, flip this value.",
    ),
    "swap_xz": (
        "Troca X e Z da velocidade local. Só se f/r no HUD não crescerem na curva (eixos da física trocados).",
        "Swaps local velocity X/Z. Only if HUD f/r don't grow in corners (physics axes swapped).",
    ),
    "reset": (
        "Volta aos valores que estavam bons no AMS2 (rate 0.55, contraesterço 0.45, deadzone 0.12, fwd_sign −1).",
        "Restores values that worked well on AMS2 (rate 0.55, countersteer 0.45, deadzone 0.12, fwd_sign −1).",
    ),
    "presets": (
        "Um arquivo por carro na pasta do jogo. O painel detecta o carro e aplica o preset dele sozinho; primeira vez usa o Stock \u2014 ajuste e clique em Salvar. Resetar apaga o preset do carro e volta ao Stock.",
        "One file per car inside the game's folder. The panel detects the car and applies its preset automatically; first time uses Stock \u2014 tune and hit Save. Reset deletes that car's preset back to Stock.",
    ),
    "btn_reset_car": ("Resetar este carro", "Reset this car"),
    "msg_no_car": ("Entre na pista com o carro pra salvar/resetar (nome detectado pelo jogo).",
                   "Drive the car first so it can be detected, then save/reset."),
    "msg_car_loaded": ("Preset do carro aplicado", "Car preset applied"),
    "msg_no_game": ("sem jogo", "no game"),
    "msg_car_reset": ("Voltou ao Stock", "Back to Stock"),
    "sel_game": ("Jogo:", "Game:"),
}


def load_ui_prefs() -> dict:
    try:
        d = json.loads(UI_PREFS_PATH.read_text())
        return d if isinstance(d, dict) else {}
    except (OSError, json.JSONDecodeError):
        return {}


def save_ui_prefs(**kw) -> None:
    d = load_ui_prefs()
    d.update(kw)
    UI_PREFS_PATH.parent.mkdir(parents=True, exist_ok=True)
    UI_PREFS_PATH.write_text(json.dumps(d))


def load_lang() -> str:
    v = load_ui_prefs().get("lang")
    return v if v in LANGS else "pt"


def save_lang(lang: str) -> None:
    save_ui_prefs(lang=lang)


def migrate_legacy_presets() -> None:
    """One-shot: legacy presets.json entries become <game>/Stock.json files."""
    try:
        data = json.loads(PRESETS_PATH.read_text())
    except (OSError, json.JSONDecodeError):
        return
    if not isinstance(data, dict):
        return
    for name, vals in data.items():
        gids = LEGACY_GAME_MAP.get(str(name))
        if not gids or not isinstance(vals, dict):
            continue
        payload = {"format": "simcontrol-preset", "version": 1,
                   "name": "Stock", "values": vals}
        for gid in gids:
            gdir = PRESETS_DIR / gid
            target = gdir / "Stock.json"
            if target.exists():
                continue
            try:
                gdir.mkdir(parents=True, exist_ok=True)
                tmp = target.with_suffix(".tmp")
                tmp.write_text(json.dumps(payload, indent=2, ensure_ascii=False))
                os.replace(tmp, target)
            except OSError:
                pass


def game_dir(gid: str) -> Path:
    return PRESETS_DIR / gid


def list_presets(gid: str) -> list:
    try:
        return sorted(p.stem for p in game_dir(gid).glob("*.json"))
    except OSError:
        return []


def preset_file_path(gid: str, name: str) -> Path:
    return game_dir(gid) / f"{name}.json"


def read_preset_file(gid: str, name: str):
    try:
        return json.loads(preset_file_path(gid, name).read_text())
    except (OSError, json.JSONDecodeError):
        return None


def write_preset_file(gid: str, name: str, values: dict) -> None:
    game_dir(gid).mkdir(parents=True, exist_ok=True)
    payload = {"format": "simcontrol-preset", "version": 1,
               "name": name, "values": values}
    p = preset_file_path(gid, name)
    tmp = p.with_suffix(".tmp")
    tmp.write_text(json.dumps(payload, indent=2, ensure_ascii=False))
    os.replace(tmp, p)


CUR_LANG = load_lang()


def tr(key: str) -> str:
    pair = STR.get(key)
    if pair:
        return pair[1] if CUR_LANG == "en" else pair[0]
    pair = HELP.get(key)
    if pair:
        return pair[1] if CUR_LANG == "en" else pair[0]
    return key


STR.update({
    "msg_enter_name": ("Digite um nome para salvar.", "Type a name to save."),
    "msg_builtin": ("“Stock” é de fábrica — escolha outro nome.",
                    "“Stock” is factory — pick another name."),
    "msg_saved": ("Salvo", "Saved"),
    "msg_loaded": ("Carregado", "Loaded"),
    "msg_deleted": ("Apagado", "Deleted"),
    "msg_choose_preset": ("Escolha ou digite um preset.", "Pick or type a preset."),
    "msg_not_found": ("Não achei o preset", "Preset not found"),
    "msg_cant_delete_builtin": ("Não dá para apagar o preset de fábrica.",
                                "The factory preset cannot be deleted."),
    "msg_exported": ("Exportado para", "Exported to"),
    "msg_imported": ("Importado", "Imported"),
    "msg_import_failed": ("Falha ao importar", "Import failed"),
    "msg_sc_already": ("simcontrol já está rodando (outro processo).",
                       "simcontrol already running (another process)."),
    "msg_sc_not_found": ("simcontrol não encontrado — rode `make` na raiz do projeto.",
                         "simcontrol not found — run `make` in the project root."),
    "msg_restarting": ("reiniciando simcontrol…", "restarting simcontrol…"),
    "msg_status_off": (
        "simcontrol não conectado — os sliders salvam o simcontrol.conf "
        "(recarrega em ~0,5 s se o simcontrol estiver rodando)",
        "simcontrol not connected — sliders still write simcontrol.conf "
        "(reloads within ~0.5 s if simcontrol is running)",
    ),
})

SC_IPC_MAGIC = 0x314C4353
SC_IPC_VERSION = 2
SC_IPC_BYTES = 4096
SHM_PATH = "/dev/shm/simcontrol_ipc"

CSS = """
window.sc { background: #161616; color: #eee; font-size: 13px; }
.sc-status { font-weight: 700; }
.sc-ok { color: #3b9fff; }
.sc-wait { color: #ffcc44; }
.sc-off { color: #ff6666; }
.sc-h { color: #3b9fff; font-weight: 700; padding-top: 8px; }
.sc-hint { color: #9aa3ad; font-size: 11px; padding-bottom: 4px; }
button { min-height: 28px; }
"""

SIMCONTROL_BIN_CANDIDATES = [
    ROOT / "simcontrol",
    Path("/usr/local/bin/simcontrol"),
    Path("/usr/bin/simcontrol"),
]
SIMCONTROL_LOG = (
    Path(os.environ.get("XDG_STATE_HOME", str(Path.home() / ".local" / "state")))
    / "simcontrol"
    / "simcontrol.log"
)


def find_simcontrol_bin() -> Path | None:
    for p in SIMCONTROL_BIN_CANDIDATES:
        if p.is_file() and os.access(p, os.X_OK):
            return p
    return None


class ScIpc(ctypes.Structure):
    _fields_ = [
        ("magic", ctypes.c_uint32),
        ("version", ctypes.c_uint32),
        ("struct_size", ctypes.c_uint32),
        ("_pad0", ctypes.c_uint32),
        ("heartbeat_ns", ctypes.c_uint64),
        ("connected", ctypes.c_int32),
        ("playing", ctypes.c_int32),
        ("assist_on", ctypes.c_int32),
        ("sc_running", ctypes.c_int32),
        ("speed_ms", ctypes.c_float),
        ("raw_steer", ctypes.c_float),
        ("final_steer", ctypes.c_float),
        ("r_axle_hvel_angle", ctypes.c_float),
        ("self_steer_strength", ctypes.c_float),
        ("front_nd_slip", ctypes.c_float),
        ("rear_nd_slip", ctypes.c_float),
        ("max_limit_reduction", ctypes.c_float),
        ("limit_reduction", ctypes.c_float),
        ("fade", ctypes.c_float),
        ("limit", ctypes.c_float),
        ("self_steer", ctypes.c_float),
        ("car", ctypes.c_char * 64),
        ("track", ctypes.c_char * 64),
        ("status", ctypes.c_char * 16),
        ("settings_ack", ctypes.c_uint32),
        ("game_id", ctypes.c_uint32),
        ("assist_enabled", ctypes.c_int32),
        ("passthrough", ctypes.c_int32),
        ("use_filter", ctypes.c_int32),
        ("graph_selection", ctypes.c_int32),
        ("filter_setting", ctypes.c_float),
        ("steering_rate", ctypes.c_float),
        ("target_slip", ctypes.c_float),
        ("rate_increase_with_speed", ctypes.c_float),
        ("self_steer_response", ctypes.c_float),
        ("damping_strength", ctypes.c_float),
        ("max_self_steer_angle", ctypes.c_float),
        ("countersteer_response", ctypes.c_float),
        ("max_dynamic_limit_reduction", ctypes.c_float),
        ("stick_gamma", ctypes.c_float),
        ("deadzone", ctypes.c_float),
        ("settings_seq", ctypes.c_uint32),
        ("save_request", ctypes.c_uint32),
        ("ui_assist_enabled", ctypes.c_int32),
        ("ui_passthrough", ctypes.c_int32),
        ("ui_use_filter", ctypes.c_int32),
        ("ui_graph_selection", ctypes.c_int32),
        ("ui_filter_setting", ctypes.c_float),
        ("ui_steering_rate", ctypes.c_float),
        ("ui_target_slip", ctypes.c_float),
        ("ui_rate_increase_with_speed", ctypes.c_float),
        ("ui_self_steer_response", ctypes.c_float),
        ("ui_damping_strength", ctypes.c_float),
        ("ui_max_self_steer_angle", ctypes.c_float),
        ("ui_countersteer_response", ctypes.c_float),
        ("ui_max_dynamic_limit_reduction", ctypes.c_float),
        ("ui_stick_gamma", ctypes.c_float),
        ("ui_deadzone", ctypes.c_float),
        ("target_slip_deg", ctypes.c_float),
        ("steering_lock_deg", ctypes.c_float),
        ("wheelbase_m", ctypes.c_float),
        ("steer_sign", ctypes.c_float),
        ("yaw_sign", ctypes.c_float),
        ("lat_sign", ctypes.c_float),
        ("fwd_sign", ctypes.c_float),
        ("swap_xz", ctypes.c_int32),
        ("invert_throttle", ctypes.c_int32),
        ("invert_brake", ctypes.c_int32),
        ("ui_target_slip_deg", ctypes.c_float),
        ("ui_steering_lock_deg", ctypes.c_float),
        ("ui_wheelbase_m", ctypes.c_float),
        ("ui_steer_sign", ctypes.c_float),
        ("ui_yaw_sign", ctypes.c_float),
        ("ui_lat_sign", ctypes.c_float),
        ("ui_fwd_sign", ctypes.c_float),
        ("ui_swap_xz", ctypes.c_int32),
        ("ui_invert_throttle", ctypes.c_int32),
        ("ui_invert_brake", ctypes.c_int32),
    ]


# ------------------------------------------------------------- presets ----

PRESET_KEYS = [
    "steering_rate",
    "rate_increase_with_speed",
    "target_slip_deg",
    "target_slip_scale",
    "self_steer_response",
    "damping_strength",
    "max_self_steer_angle",
    "countersteer_response",
    "max_dynamic_limit_reduction",
    "steering_lock_deg",
    "wheelbase_m",
    "stick_gamma",
    "deadzone",
    "steer_sign",
    "yaw_sign",
    "lat_sign",
    "fwd_sign",
    "swap_xz",
    "invert_throttle",
    "invert_brake",
]


def settings_to_preset(s: Settings) -> dict:
    out = {}
    for k in PRESET_KEYS:
        v = getattr(s, k)
        out[k] = int(v) if isinstance(v, int) else float(v)
    return out


def apply_preset(s: Settings, data: dict) -> None:
    for k in PRESET_KEYS:
        if k not in data:
            continue
        cur = getattr(s, k)
        try:
            if isinstance(cur, int) and not isinstance(cur, bool):
                setattr(s, k, int(data[k]))
            else:
                setattr(s, k, float(data[k]))
        except (TypeError, ValueError):
            pass


def load_presets() -> dict:
    try:
        data = json.loads(PRESETS_PATH.read_text())
        if isinstance(data, dict):
            return {str(k): v for k, v in data.items() if isinstance(v, dict)}
    except (OSError, json.JSONDecodeError):
        pass
    return {}


def save_presets(presets: dict) -> None:
    tmp = PRESETS_PATH.with_suffix(".tmp")
    tmp.write_text(json.dumps(presets, indent=2, ensure_ascii=False))
    os.replace(tmp, PRESETS_PATH)


@dataclass
class Settings:
    assist_enabled: int = 1
    passthrough: int = 0
    grab: int = 1
    loop_hz: int = 250
    hud: int = 1
    use_filter: int = 0
    graph_selection: int = 1
    filter_setting: float = 0.5
    steering_rate: float = 0.55
    rate_increase_with_speed: float = 0.0
    target_slip_deg: float = 7.0
    target_slip_scale: float = 0.95
    self_steer_response: float = 0.37
    damping_strength: float = 0.37
    max_self_steer_angle: float = 90.0
    countersteer_response: float = 0.45
    max_dynamic_limit_reduction: float = 5.0
    steering_lock_deg: float = 20.0
    wheelbase_m: float = 2.60
    stick_gamma: float = 1.40
    deadzone: float = 0.12
    steer_sign: float = 1.0
    yaw_sign: float = 1.0
    lat_sign: float = 1.0
    fwd_sign: float = -1.0
    swap_xz: int = 0
    invert_throttle: int = 0
    invert_brake: int = 0
    gamepad_name: str = ""
    shm_path: str = ""
    udp_port: int = 5606


def find_conf() -> Path:
    for p in CONF_CANDIDATES:
        if p.is_file():
            return p
    ROOT.mkdir(parents=True, exist_ok=True)
    return ROOT / "simcontrol.conf"


def load_conf(path: Path) -> Settings:
    s = Settings()
    if not path.is_file():
        return s
    names = {f.name for f in fields(s)}
    for line in path.read_text(errors="replace").splitlines():
        line = re.sub(r"#.*", "", line).strip()
        if "=" not in line:
            continue
        k, v = [x.strip() for x in line.split("=", 1)]
        if k not in names:
            continue
        cur = getattr(s, k)
        if isinstance(cur, str):
            setattr(s, k, v)
        elif isinstance(cur, int) and not isinstance(cur, bool):
            try:
                setattr(s, k, int(float(v)))
            except ValueError:
                pass
        else:
            try:
                setattr(s, k, float(v))
            except ValueError:
                pass
    return s


def save_conf(path: Path, s: Settings) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        f"""# SimControl — steering assist (AMS2 / AC Evo / AC Rally / PCARS 2 /
# RaceRoom / AMS1 / rFactor 2). Written by the config panel; simcontrol
# hot-reloads this file automatically.

assist_enabled = {int(s.assist_enabled)}
passthrough    = {int(s.passthrough)}
grab           = {int(s.grab)}
loop_hz        = {int(s.loop_hz)}
hud            = {int(s.hud)}
use_filter     = 0
filter_setting = {s.filter_setting:.4f}
graph_selection = {int(s.graph_selection)}

gamepad_name = {s.gamepad_name}
shm_path = {s.shm_path}
udp_port = {int(s.udp_port)}

steering_rate                = {s.steering_rate:.4f}
rate_increase_with_speed     = {s.rate_increase_with_speed:.4f}
target_slip_deg              = {s.target_slip_deg:.4f}
target_slip_scale            = {s.target_slip_scale:.4f}
self_steer_response          = {s.self_steer_response:.4f}
damping_strength             = {s.damping_strength:.4f}
max_self_steer_angle         = {s.max_self_steer_angle:.4f}
countersteer_response        = {s.countersteer_response:.4f}
max_dynamic_limit_reduction  = {s.max_dynamic_limit_reduction:.4f}

steering_lock_deg            = {s.steering_lock_deg:.4f}
wheelbase_m                  = {s.wheelbase_m:.4f}

stick_gamma                  = {s.stick_gamma:.4f}
deadzone                     = {s.deadzone:.4f}

steer_sign = {s.steer_sign:.0f}
yaw_sign   = {s.yaw_sign:.0f}
lat_sign   = {s.lat_sign:.0f}
fwd_sign   = {s.fwd_sign:.0f}
swap_xz    = {int(s.swap_xz)}

invert_throttle = {int(s.invert_throttle)}
invert_brake    = {int(s.invert_brake)}
"""
    )


class Ipc:
    def __init__(self) -> None:
        self.mm = None
        self.ipc: ScIpc | None = None
        try:
            fd = os.open(SHM_PATH, os.O_RDWR | os.O_CREAT, 0o666)
            os.ftruncate(fd, SC_IPC_BYTES)
            self.mm = mmap_mod.mmap(fd, SC_IPC_BYTES)
            os.close(fd)
            self.ipc = ScIpc.from_buffer(self.mm)
        except OSError:
            self.ipc = None

    def alive(self) -> bool:
        ipc = self.ipc
        if ipc is None:
            return False
        if ipc.magic != SC_IPC_MAGIC or ipc.version != SC_IPC_VERSION:
            return False
        if ipc.struct_size != ctypes.sizeof(ScIpc):
            return False
        if not ipc.sc_running or ipc.heartbeat_ns == 0:
            return False
        now = time.monotonic_ns()
        age = now - ipc.heartbeat_ns if now >= ipc.heartbeat_ns else 0
        return age < 800_000_000

    def push(self, s: Settings) -> None:
        ipc = self.ipc
        if ipc is None:
            return
        ipc.ui_assist_enabled = int(s.assist_enabled)
        ipc.ui_passthrough = int(s.passthrough)
        ipc.ui_use_filter = 0
        ipc.ui_graph_selection = int(s.graph_selection)
        ipc.ui_filter_setting = float(s.filter_setting)
        ipc.ui_steering_rate = float(s.steering_rate)
        ipc.ui_target_slip = float(s.target_slip_scale)
        ipc.ui_rate_increase_with_speed = float(s.rate_increase_with_speed)
        ipc.ui_self_steer_response = float(s.self_steer_response)
        ipc.ui_damping_strength = float(s.damping_strength)
        ipc.ui_max_self_steer_angle = float(s.max_self_steer_angle)
        ipc.ui_countersteer_response = float(s.countersteer_response)
        ipc.ui_max_dynamic_limit_reduction = float(s.max_dynamic_limit_reduction)
        ipc.ui_stick_gamma = float(s.stick_gamma)
        ipc.ui_deadzone = float(s.deadzone)
        ipc.ui_target_slip_deg = float(s.target_slip_deg)
        ipc.ui_steering_lock_deg = float(s.steering_lock_deg)
        ipc.ui_wheelbase_m = float(s.wheelbase_m)
        ipc.ui_steer_sign = float(s.steer_sign)
        ipc.ui_yaw_sign = float(s.yaw_sign)
        ipc.ui_lat_sign = float(s.lat_sign)
        ipc.ui_fwd_sign = float(s.fwd_sign)
        ipc.ui_swap_xz = int(s.swap_xz)
        ipc.ui_invert_throttle = int(s.invert_throttle)
        ipc.ui_invert_brake = int(s.invert_brake)
        ipc.save_request = 1
        ipc.settings_seq = ipc.settings_seq + 1


def _hint(text: str) -> Gtk.Label:
    lab = Gtk.Label(label=text, xalign=0, wrap=True)
    lab.add_css_class("sc-hint")
    lab.set_max_width_chars(42)
    return lab


class SliderRow(Gtk.Box):
    def __init__(self, key, label, lo, hi, step, fmt, on_change, help_text=""):
        super().__init__(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        self.key = key
        self.fmt = fmt
        self.on_change = on_change
        self._sync = False
        top = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL)
        self.lab = Gtk.Label(label=label, xalign=0)
        self.lab.set_hexpand(True)
        self.val = Gtk.Label(xalign=1)
        top.append(self.lab)
        top.append(self.val)
        self.append(top)
        self.scale = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, lo, hi, step)
        self.scale.set_draw_value(False)
        self.scale.set_hexpand(True)
        self.scale.connect("value-changed", self._moved)
        self.append(self.scale)
        self.hint = None
        if help_text:
            self.set_tooltip_text(help_text)
            self.hint = _hint(help_text)
            self.append(self.hint)

    def _moved(self, scale):
        if self._sync:
            return
        v = scale.get_value()
        self.val.set_text(self.fmt % v)
        self.on_change(self.key, v)

    def set_value(self, v: float) -> None:
        self._sync = True
        self.scale.set_value(v)
        self.val.set_text(self.fmt % v)
        self._sync = False


class SignRow(Gtk.Box):
    def __init__(self, key, label, on_change, minus_is_typical=False, help_text=""):
        super().__init__(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        self.key = key
        self.on_change = on_change
        self._sync = False
        row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        self.lab = Gtk.Label(label=label, xalign=0, hexpand=True)
        row.append(self.lab)
        self.plus = Gtk.CheckButton(label="+1")
        self.minus = Gtk.CheckButton(label="-1")
        self.minus.set_group(self.plus)
        if minus_is_typical:
            self.minus.set_active(True)
        else:
            self.plus.set_active(True)
        self.plus.connect("toggled", self._tog)
        row.append(self.plus)
        row.append(self.minus)
        self.append(row)
        self.hint = None
        if help_text:
            self.set_tooltip_text(help_text)
            self.hint = _hint(help_text)
            self.append(self.hint)

    def _tog(self, *_a):
        if self._sync:
            return
        self.on_change(self.key, 1.0 if self.plus.get_active() else -1.0)

    def set_value(self, v: float) -> None:
        self._sync = True
        if v < 0:
            self.minus.set_active(True)
        else:
            self.plus.set_active(True)
        self._sync = False


class ScWindow(Gtk.ApplicationWindow):
    def __init__(self, app: "App"):
        super().__init__(title=tr("win_title"), application=app)
        self.app = app
        self.set_default_size(380, 720)
        self.add_css_class("sc")
        self._sync = False
        self._sync_presets = False
        self.gid = load_ui_prefs().get("game_group") or GAME_GROUPS[0][0]

        scroll = Gtk.ScrolledWindow()
        scroll.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
        self.set_child(scroll)
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4)
        box.set_margin_top(10)
        box.set_margin_bottom(12)
        box.set_margin_start(12)
        box.set_margin_end(12)
        scroll.set_child(box)

        # language selector (rebuilds window on change)
        lang_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        lang_row.append(Gtk.Label(label=tr("lang_label"), xalign=0, hexpand=True))
        store = Gtk.StringList.new(["Português", "English"])
        drop = Gtk.DropDown(model=store)
        drop.set_selected(0 if CUR_LANG == "pt" else 1)
        drop.connect("notify::selected", self._lang_changed)
        lang_row.append(drop)
        box.append(lang_row)

        self.status = Gtk.Label(xalign=0, wrap=True)
        self.status.add_css_class("sc-status")
        self.status.set_ellipsize(Pango.EllipsizeMode.END)
        box.append(self.status)
        self.conf_lab = Gtk.Label(xalign=0)
        self.conf_lab.add_css_class("sc-wait")
        box.append(self.conf_lab)

        sc_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        b_restart = Gtk.Button(label=tr("restart_btn"))
        b_restart.set_tooltip_text(tr("restart_tip"))
        b_restart.connect("clicked", lambda *_a: self.app.restart_simcontrol())
        b_restart.set_hexpand(True)
        sc_row.append(b_restart)
        box.append(sc_row)
        self.sc_msg = Gtk.Label(xalign=0, wrap=True)
        self.sc_msg.add_css_class("sc-hint")
        box.append(self.sc_msg)

        def hdr(t):
            l = Gtk.Label(label=t, xalign=0)
            l.add_css_class("sc-h")
            box.append(l)

        self.checks = {}
        self.sliders = {}
        self.signs = {}

        def chk(key, skey):
            wrap = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
            c = Gtk.CheckButton(label=tr(skey))
            c.connect("toggled", lambda w, k=key: self._bool(k, w.get_active()))
            tip = tr(key)
            c.set_tooltip_text(tip)
            wrap.append(c)
            wrap.append(_hint(tip))
            box.append(wrap)
            self.checks[key] = c

        def sl(key, skey, lo, hi, step, fmt):
            row = SliderRow(key, tr(skey), lo, hi, step, fmt, self._float, tr(key))
            box.append(row)
            self.sliders[key] = row

        def sg(key, skey, minus=False):
            row = SignRow(key, tr(skey), self._float, minus_is_typical=minus, help_text=tr(key))
            box.append(row)
            self.signs[key] = row

        hdr(tr("hdr_geral"))
        chk("assist_enabled", "chk_assist_enabled")
        chk("passthrough", "chk_passthrough")

        hdr(tr("hdr_presets"))
        box.append(_hint(tr("presets")))
        prow = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=4)
        b_save = Gtk.Button(label=tr("btn_save"))
        b_rst = Gtk.Button(label=tr("btn_reset_car"))
        for btn in (b_save, b_rst):
            btn.set_hexpand(True)
        b_save.connect("clicked", self._preset_save)
        b_rst.connect("clicked", self._preset_reset_car)
        prow.append(b_save)
        prow.append(b_rst)
        box.append(prow)
        irow = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=4)
        b_exp = Gtk.Button(label=tr("btn_export"))
        b_imp = Gtk.Button(label=tr("btn_import"))
        b_exp.set_hexpand(True)
        b_imp.set_hexpand(True)
        b_exp.connect("clicked", self._preset_export)
        b_imp.connect("clicked", self._preset_import)
        irow.append(b_exp)
        irow.append(b_imp)
        box.append(irow)
        self.preset_msg = Gtk.Label(xalign=0, wrap=True)
        self.preset_msg.add_css_class("sc-hint")
        box.append(self.preset_msg)

        hdr(tr("hdr_steer"))
        sl("steering_rate", "sl_steering_rate", 0.05, 1.0, 0.01, "%.2f")
        sl("rate_increase_with_speed", "sl_rate_increase_with_speed", -0.5, 0.5, 0.01, "%+.2f")
        sl("target_slip_deg", "sl_target_slip_deg", 3.0, 14.0, 0.1, "%.1f°")
        sl("target_slip_scale", "sl_target_slip_scale", 0.80, 1.20, 0.01, "%.2f")
        sl("countersteer_response", "sl_countersteer_response", 0.0, 1.0, 0.01, "%.2f")
        sl("max_dynamic_limit_reduction", "sl_max_dynamic_limit_reduction", 0.0, 10.0, 0.1, "%.1f")

        hdr(tr("hdr_selfsteer"))
        sl("self_steer_response", "sl_self_steer_response", 0.0, 1.0, 0.01, "%.2f")
        sl("max_self_steer_angle", "sl_max_self_steer_angle", 0.0, 90.0, 0.5, "%.1f°")
        sl("damping_strength", "sl_damping_strength", 0.0, 1.0, 0.01, "%.2f")

        hdr(tr("hdr_pad"))
        sl("stick_gamma", "sl_stick_gamma", 0.8, 2.5, 0.05, "%.2f")
        sl("deadzone", "sl_deadzone", 0.0, 0.40, 0.01, "%.2f")
        chk("invert_throttle", "chk_invert_throttle")
        chk("invert_brake", "chk_invert_brake")

        hdr(tr("hdr_model"))
        sl("steering_lock_deg", "sl_steering_lock_deg", 8.0, 40.0, 0.5, "%.1f°")
        sl("wheelbase_m", "sl_wheelbase_m", 1.8, 3.4, 0.05, "%.2f m")
        sg("steer_sign", "sg_steer_sign")
        sg("lat_sign", "sg_lat_sign")
        sg("yaw_sign", "sg_yaw_sign")
        sg("fwd_sign", "sg_fwd_sign", minus=True)
        chk("swap_xz", "chk_swap_xz")

        reset = Gtk.Button(label=tr("btn_reset"))
        reset.set_tooltip_text(tr("reset"))
        reset.connect("clicked", self._reset)
        box.append(reset)
        box.append(_hint(tr("reset")))

        self.sync_from_settings()
        self.conf_lab.set_text(f"{app.conf_path}")

    # ------------------------------------------------------------ i18n --

    def _lang_changed(self, drop, _pspec):
        idx = int(drop.get_selected())
        new_lang = "pt" if idx == 0 else "en"
        if new_lang == CUR_LANG:
            return
        globals()["CUR_LANG"] = new_lang
        save_lang(new_lang)
        self.app.rebuild_ui()

    # ---------------------------------------------------------- basics --

    def _bool(self, key, val):
        if self._sync:
            return
        setattr(self.app.settings, key, 1 if val else 0)
        self.app.changed()

    def _float(self, key, val):
        if self._sync:
            return
        setattr(self.app.settings, key, val)
        self.app.changed()

    def _reset(self, *_a):
        self.app.settings = Settings()
        self.sync_from_settings()
        self.app.changed()
        self.preset_msg.set_text(f"{tr('btn_load')}: {BUILTIN_PRESET}")

    # --------------------------------------------------------- presets --

    def _current_car(self) -> str:
        """Sanitized detected car name, usable as a preset file name."""
        ipc = self.app.ipc.ipc
        if not self.app.ipc.alive() or ipc is None:
            return ""
        raw = ipc.car.split(b"\x00", 1)[0].decode("utf-8", "replace")
        car = "".join(c for c in raw
                      if c.isalnum() or c in "-_ .()[]").strip().strip(".")[:48]
        return "" if car.lower() in ("", "?", "(no car)") else car

    def _resolve_gid(self, car: str) -> str | None:
        """Folder for this car: live game id first, then existing files."""
        ipc = self.app.ipc.ipc
        if self.app.ipc.alive() and ipc is not None:
            gid = SRC2GID.get(int(ipc.game_id))
            if gid:
                return gid
        for gid, _lbl in GAME_GROUPS:
            if preset_file_path(gid, car).exists():
                return gid
        return None

    @staticmethod
    def _vals_from(data: dict) -> dict | None:
        try:
            _pn, vals = ScWindow._extract_preset(data)
        except ValueError:
            vals = data if isinstance(data, dict) else None
        return vals

    def _preset_picked(self, *_a) -> None:
        pass

    def _preset_save(self, *_a) -> None:
        car = self._current_car()
        if not car:
            self.preset_msg.set_text(tr("msg_no_car"))
            return
        gid = self._resolve_gid(car) or self.gid
        write_preset_file(gid, car, settings_to_preset(self.app.settings))
        save_ui_prefs(game_group=gid)
        self.gid = gid
        self.preset_msg.set_text(f"{tr('msg_saved')}: {gid}/{car}")

    def _preset_reset_car(self, *_a) -> None:
        car = self._current_car()
        if not car:
            self.preset_msg.set_text(tr("msg_no_car"))
            return
        gid = self._resolve_gid(car) or self.gid
        for g, _l in GAME_GROUPS:
            pth = preset_file_path(g, car)
            if pth.exists():
                pth.unlink()
        data = read_preset_file(gid, "Stock")
        if data:
            vals = ScWindow._vals_from(data)
            if vals:
                apply_preset(self.settings, vals)
                self.gid = gid
        else:
            self.app.settings = Settings()
        self.sync_from_settings()
        self.app.changed()
        self.preset_msg.set_text(f"{tr('msg_car_reset')}: {gid}/Stock")

    # ----------------------------------------------- preset file io -----

    @staticmethod
    def _extract_preset(data: dict) -> tuple[str, dict]:
        """Accept our export shape or plain flat dicts."""
        if not isinstance(data, dict):
            raise ValueError("not a dict")
        vals = data.get("values") if isinstance(data.get("values"), dict) else None
        name = str(data.get("name") or "")
        if vals is None:
            keys = set(data.keys()) & set(PRESET_KEYS)
            if keys:
                vals = data
            else:
                only_one = [v for v in data.values() if isinstance(v, dict)]
                if len(only_one) == 1:
                    vals = only_one[0]
        if not isinstance(vals, dict):
            raise ValueError("no preset values found")
        if not any(k in vals for k in PRESET_KEYS):
            raise ValueError("no known keys")
        return name, vals

    def _preset_export(self, *_a) -> None:
        name = self._current_car() or "simcontrol-preset"
        payload = {
            "format": "simcontrol-preset",
            "version": 1,
            "name": name,
            "values": settings_to_preset(self.app.settings),
        }
        dlg = Gtk.FileDialog()
        dlg.set_initial_name(f"{name}.json")
        dlg.save(self, None, self._export_done, payload)

    def _export_done(self, dlg, res, payload):
        try:
            f = dlg.save_finish(res)
        except GLib.Error:
            return  # cancelled
        path = f.get_path()
        if not path:
            return
        try:
            Path(path).write_text(json.dumps(payload, indent=2, ensure_ascii=False))
            self.preset_msg.set_text(f"{tr('msg_exported')}: {path}")
        except OSError as e:
            self.preset_msg.set_text(f"export: {e}")

    def _preset_import(self, *_a) -> None:
        dlg = Gtk.FileDialog()
        flt = Gtk.FileFilter()
        flt.set_name("JSON")
        flt.add_pattern("*.json")
        dlg.set_default_filter(flt)
        dlg.open(self, None, self._import_done)

    def _import_done(self, dlg, res, _u=None):
        try:
            f = dlg.open_finish(res)
        except GLib.Error:
            return  # cancelled
        path = f.get_path()
        if not path:
            return
        try:
            raw = Path(path).read_text()
            pname, vals = self._extract_preset(json.loads(raw))
        except (OSError, ValueError, json.JSONDecodeError) as e:
            self.preset_msg.set_text(f"{tr('msg_import_failed')}: {e}")
            return
        name = pname or Path(path).stem or "imported"
        apply_preset(self.app.settings, vals)
        self.sync_from_settings()
        self.app.changed()
        car = self._current_car()
        if car:
            car_gid = self._resolve_gid(car) or self.gid
            write_preset_file(car_gid, car,
                              settings_to_preset(self.app.settings))
            save_ui_prefs(game_group=car_gid)
            self.gid = car_gid
            name = f"{car_gid}/{car}"
        self.preset_msg.set_text(f"{tr('msg_imported')}: {name}")

    # -------------------------------------------------------------- misc -

    def sync_from_settings(self):
        self._sync = True
        s = self.app.settings
        for k, w in self.checks.items():
            w.set_active(bool(getattr(s, k)))
        for k, w in self.sliders.items():
            w.set_value(float(getattr(s, k)))
        for k, w in self.signs.items():
            w.set_value(float(getattr(s, k)))
        self._sync = False

    def set_status(self, text, kind):
        self.status.set_text(text)
        for c in ("sc-ok", "sc-wait", "sc-off"):
            self.status.remove_css_class(c)
        self.status.add_css_class({"ok": "sc-ok", "wait": "sc-wait", "off": "sc-off"}[kind])

    def set_sc_msg(self, text: str) -> None:
        self.sc_msg.set_text(text)


class App(Gtk.Application):
    def __init__(self):
        super().__init__(application_id="dev.simcontrol.config")
        self.conf_path = find_conf()
        self.settings = load_conf(self.conf_path)
        self.ipc = Ipc()
        self.win: ScWindow | None = None
        self._save_src = 0
        self.sc_bin = find_simcontrol_bin()
        self.sc_proc: subprocess.Popen | None = None
        self._sc_restart_src = 0
        self._car_applied: str | None = None

    def do_activate(self):
        prov = Gtk.CssProvider()
        prov.load_from_data(CSS.encode())
        Gtk.StyleContext.add_provider_for_display(
            Gdk.Display.get_default(), prov, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION
        )
        try:
            Gtk.Settings.get_default().set_property("gtk-application-prefer-dark-theme", True)
        except Exception:
            pass
        if self.win is None:
            self.win = ScWindow(self)
            self.win.connect("close-request", self._on_close)
        self.win.present()
        GLib.timeout_add(120, self._tick)
        # Opening the panel starts simcontrol if none is running.
        if not self.ipc.alive():
            self.start_simcontrol()

    def rebuild_ui(self):
        """Recreate the window so every label follows the new language."""
        if self.win is not None:
            self.win.destroy()
        self.win = None
        self.do_activate()

    def start_simcontrol(self):
        if self.sc_proc is not None and self.sc_proc.poll() is None:
            return  # ours and alive
        if self.ipc.alive():
            if self.win:
                self.win.set_sc_msg(tr("msg_sc_already"))
            return
        if self.sc_bin is None:
            if self.win:
                self.win.set_sc_msg(tr("msg_sc_not_found"))
            return
        try:
            SIMCONTROL_LOG.parent.mkdir(parents=True, exist_ok=True)
            logf = open(SIMCONTROL_LOG, "ab")
            self.sc_proc = subprocess.Popen(
                [str(self.sc_bin), "-c", str(self.conf_path)],
                stdout=logf,
                stderr=logf,
                stdin=subprocess.DEVNULL,
                cwd=str(self.sc_bin.parent),
                start_new_session=True,
            )
            if self.win:
                self.win.set_sc_msg(
                    f"simcontrol pid {self.sc_proc.pid} — {SIMCONTROL_LOG}"
                )
        except OSError as e:
            if self.win:
                self.win.set_sc_msg(f"start: {e}")

    def stop_simcontrol(self):
        proc = self.sc_proc
        if proc is not None and proc.poll() is None:
            try:
                proc.send_signal(signal.SIGTERM)
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=2)
            except Exception:
                pass
            self.sc_proc = None
            return
        # Not started by this panel (terminal or Steam launch options) —
        # kill it by binary name.
        if self.sc_bin is not None:
            try:
                subprocess.run(["pkill", "-TERM", "-f", str(self.sc_bin)], check=False)
            except Exception:
                pass

    def restart_simcontrol(self):
        self._car_applied = None  # re-run car preset detection after restart
        if self.win:
            self.win.set_sc_msg(tr("msg_restarting"))
        self.stop_simcontrol()
        if self._sc_restart_src:
            GLib.source_remove(self._sc_restart_src)
        self._sc_restart_src = GLib.timeout_add(500, self._start_after_stop)

    def _start_after_stop(self):
        self._sc_restart_src = 0
        self.start_simcontrol()
        return False

    def changed(self):
        self.ipc.push(self.settings)
        if self._save_src:
            GLib.source_remove(self._save_src)
        self._save_src = GLib.timeout_add(250, self._save)

    def _save(self):
        self._save_src = 0
        save_conf(self.conf_path, self.settings)
        self.ipc.push(self.settings)
        if self.win:
            self.win.conf_lab.set_text(f"✓ {self.conf_path.name}")
        return False

    def _on_close(self, *_a):
        save_conf(self.conf_path, self.settings)
        self.ipc.push(self.settings)
        return False

    def _tick(self):
        if not self.win:
            return True
        ipc = self.ipc.ipc
        if self.ipc.alive() and ipc is not None:
            car = ipc.car.split(b"\x00", 1)[0].decode("utf-8", "replace") or "?"
            st = ipc.status.split(b"\x00", 1)[0].decode("ascii", "replace") or "?"
            kmh = ipc.speed_ms * 3.6
            game = SRC2LABEL.get(int(ipc.game_id)) or tr("msg_no_game")
            self.win.set_status(
                f"{game}  |  simcontrol {st}  |  {car}  |  {kmh:.0f} km/h  |  "
                f"in{ipc.raw_steer:+.2f} out{ipc.final_steer:+.2f} ss{ipc.self_steer:+.2f}",
                "ok" if ipc.assist_on else "wait",
            )
        else:
            self.win.set_status(tr("msg_status_off"), "off")
        self._maybe_autoload()
        return True

    def _maybe_autoload(self):
        """Detect the current car and apply its per-game preset once."""
        ipc = self.ipc.ipc
        if not self.ipc.alive() or ipc is None or self.win is None:
            return
        raw = ipc.car.split(b"\x00", 1)[0].decode("utf-8", "replace")
        car = "".join(c for c in raw
                      if c.isalnum() or c in "-_ .()[]").strip().strip(".")[:48]
        if not car or car == self._car_applied:
            return
        self._car_applied = car
        gid = self.win._resolve_gid(car)
        data = read_preset_file(gid, car) if gid else None
        if data is None and gid:
            data = read_preset_file(gid, "Stock")  # first time in this car
        if data is None and gid is None:
            stock_gid = None
            for g, _l in GAME_GROUPS:
                d0 = read_preset_file(g, "Stock")
                if d0:
                    gid, data, stock_gid = g, d0, g
                    break
        if data:
            vals = ScWindow._vals_from(data)
            if vals:
                apply_preset(self.settings, vals)
                self.win.sync_from_settings()
                self.changed()
                if gid and f"/{car}" not in str(data.get("name", "")) \
                        and not preset_file_path(gid, car).exists():
                    tag = "Stock"
                else:
                    tag = car
                save_ui_prefs(game_group=gid)
                self.win.gid = gid
                self.win.set_sc_msg(
                    f"{tr('msg_car_loaded')}: {gid}/{tag} ({car})")
                return
        self.win.set_sc_msg(f"{tr('presets')} \u2014 Stock ({car})")


def bootstrap_stock_presets() -> None:
    """Factory-default Stock for any game folder that lacks one."""
    from dataclasses import fields as _dcf
    defaults = {f.name: getattr(Settings(), f.name) for f in fields(Settings())
                if f.name in PRESET_KEYS}
    for gid, _lbl in GAME_GROUPS:
        target = PRESETS_DIR / gid / "Stock.json"
        if target.exists():
            continue
        try:
            write_preset_file(gid, "Stock", defaults)
        except OSError:
            pass


def main():
    migrate_legacy_presets()
    bootstrap_stock_presets()
    print(f"simcontrol-config: conf {find_conf()}  ipc {ctypes.sizeof(ScIpc)} bytes  lang {CUR_LANG}")
    return App().run()


if __name__ == "__main__":
    raise SystemExit(main())
