//! Per-window bridge; the copy state machine lives in the native terminal core.
use std::cell::RefCell;
use std::collections::{HashMap, HashSet};
use std::io::{self, BufRead, BufReader, Write};
use std::os::unix::net::{UnixDatagram, UnixStream};
use std::path::PathBuf;
use std::sync::mpsc::{self, SyncSender};
use std::time::{Duration, Instant};

use serde_json::{Value, json};
use winit::event::{ElementState, KeyEvent, MouseScrollDelta};
use winit::keyboard::{Key, ModifiersState, NamedKey, PhysicalKey};
use winit::window::WindowId;

use alacritty_terminal::term::{ClipboardType, Term};
use alacritty_terminal::term::copy::{Effect, View};

use crate::clipboard::Clipboard;
use crate::config::{UiConfig, ui_config::HintAction};
use crate::display::hint;
use crate::event::{Event, EventProxy, EventType};
use crate::scheduler::{Scheduler, TimerId, Topic};

const COPY_FEEDBACK: Duration = Duration::from_millis(50);

thread_local! {
    static REMEMBERED: RefCell<HashMap<(PathBuf, i64), (View, Option<String>)>> = RefCell::new(HashMap::new());
}

pub struct Controller {
    pub view: Option<View>,
    pane: i64,
    sender: SyncSender<(u64, Value)>,
    proxy: EventProxy,
    serial: u64,
    pending: Option<u64>,
    consumed: HashSet<PhysicalKey>,
    incarnation: Option<String>,
    presentation: u64,
    focused: bool,
    exit_at: Option<Instant>,
    scope: PathBuf,
    remembered: Option<(View, Option<String>)>,
    wheel_remainder: f64,
    scrolling: bool,
}

fn wheel_rows(delta: MouseScrollDelta, cell_height: f64, remainder: &mut f64) -> i32 {
    let rows = match delta {
        MouseScrollDelta::LineDelta(_, y) => f64::from(y) * 3.,
        MouseScrollDelta::PixelDelta(point) => point.y / cell_height.max(1.),
    };
    if !rows.is_finite() { return 0; }
    *remainder += rows.clamp(-128., 128.);
    let whole = remainder.trunc() as i32;
    *remainder -= f64::from(whole);
    whole
}

impl Drop for Controller {
    fn drop(&mut self) {
        let saved = if self.exit_at.is_none() {
            self.view.take().map(|mut view| {
                view.suspend();
                (view, self.incarnation.take())
            }).or_else(|| self.remembered.take())
        } else { None };
        if let Some(saved) = saved {
            REMEMBERED.with(|views| views.borrow_mut().insert((self.scope.clone(), self.pane), saved));
        }
    }
}

fn synchronize_view<T>(view: &mut Option<View>, live: &Term<T>, presentation: &mut u64,
    pending: &mut Option<u64>, focused: &mut bool, exit_at: &mut Option<Instant>) {
    if *presentation != live.amux_copy_reset {
        if let Some(view) = view { view.suspend(); }
        *pending = None;
        *presentation = live.amux_copy_reset;
    }
    if exit_at.is_none() {
        if let Some(old) = view.as_ref().filter(|view| view.resized(&live.amux_history)) {
            let relocation = old.carry();
            let mut replacement = View::new(live);
            replacement.relocate(relocation);
            *view = Some(replacement); *pending = None;
        }
    }
    if view.as_ref().is_some_and(|view| !view.valid(&live.amux_history) && !view.superseded(&live.amux_history)) {
        *view = None; *pending = None; *exit_at = None;
    }
    if let Some(view) = view {
        if *focused && !live.is_focused { view.blur(); }
        view.update(&live.amux_history);
        view.terminal.lock().is_focused = live.is_focused;
    }
    *focused = live.is_focused;
}

fn read(reader: &mut BufReader<UnixStream>, value: Value) -> io::Result<Value> {
    serde_json::to_writer(reader.get_mut(), &value)?;
    reader.get_mut().write_all(b"\n")?;
    let mut bytes = Vec::new();
    loop {
        let buf = reader.fill_buf()?;
        if buf.is_empty() { return Err(io::Error::other("history relay disconnected")); }
        let count = buf.iter().position(|b| *b == b'\n').map_or(buf.len(), |i| i + 1);
        bytes.extend_from_slice(&buf[..count]);
        reader.consume(count);
        if bytes.len() > 1048576 { return Err(io::Error::other("history response exceeds byte budget")); }
        if bytes.last() == Some(&b'\n') { break; }
    }
    serde_json::from_slice(&bytes).map_err(io::Error::other)
}

fn link_command<T>(term: &Term<T>, config: &UiConfig) -> Option<(String, Vec<String>)> {
    let hint = hint::highlighted_at(term, config, term.vi_mode_cursor.point, ModifiersState::all())?;
    let HintAction::Command(command) = hint.action() else { return None; };
    let mut args = command.args().to_vec();
    args.push(hint.text(term)?.into_owned());
    Some((command.program().to_owned(), args))
}

impl Controller {
    pub fn new(class: Option<&str>, proxy: EventProxy) -> Option<Self> {
        if std::env::var_os("AMUX_STATE_STREAM").is_none() { return None; }
        let pane: i64 = class?.strip_prefix("amux-pane-")?.parse().ok()?;
        let scope = PathBuf::from(std::env::var_os("XDG_RUNTIME_DIR")?);
        let remembered = REMEMBERED.with(|views| views.borrow_mut().remove(&(scope.clone(), pane)));
        let path = scope.join(format!("history-{pane}.sock"));
        let (sender, receiver) = mpsc::sync_channel::<(u64, Value)>(1);
        let worker_proxy = proxy.clone();
        std::thread::spawn(move || {
            let mut reader = None;
            while let Ok((serial, request)) = receiver.recv() {
                let result = (|| {
                    if reader.is_none() {
                        let stream = UnixStream::connect(&path)?;
                        stream.set_read_timeout(Some(Duration::from_secs(3)))?;
                        stream.set_write_timeout(Some(Duration::from_secs(3)))?;
                        reader = Some(BufReader::new(stream));
                    }
                    read(reader.as_mut().unwrap(), request)
                })();
                let failed = result.is_err();
                let result = result.unwrap_or_else(|error: io::Error| json!({"error":error.to_string()}));
                worker_proxy.send_event(EventType::AmuxHistory(serial, result));
                if failed { break; }
            }
        });
        Some(Self { view: None, pane, sender, proxy, serial: 0, pending: None,
            consumed: HashSet::new(), incarnation: None, presentation: 0, focused: false, exit_at: None,
            scope, remembered, wheel_remainder: 0., scrolling: false })
    }

    pub fn open_link(&mut self, program: &str, args: &[String]) {
        let result = (|| -> io::Result<()> {
            let path = std::env::var_os("AMUX_HOST_OPEN_SOCKET")
                .ok_or_else(|| io::Error::other("host opener unavailable"))?;
            let request = serde_json::to_vec(&json!({"pane": self.pane, "argv":
                std::iter::once(program.to_owned()).chain(args.iter().cloned()).collect::<Vec<_>>()}))?;
            if request.len() > 65536 { return Err(io::Error::other("link command exceeds byte budget")); }
            let socket = UnixDatagram::unbound()?;
            socket.set_nonblocking(true)?;
            socket.send_to(&request, path)?;
            Ok(())
        })();
        if let Some(view) = &mut self.view {
            view.status = match result {
                Ok(()) => "Opening link".into(),
                Err(error) => format!("Cannot open link: {error}"),
            };
            view.notice();
        }
    }

    pub fn key(&mut self, event: &KeyEvent, modifiers: ModifiersState, live: &Term<EventProxy>) -> bool {
        self.synchronize(live);
        self.scrolling = false;
        if event.state == ElementState::Released { return self.consumed.remove(&event.physical_key); }
        if self.exit_at.take().is_some() { self.view = None; self.pending = None; }
        if event.repeat && self.view.is_none() && self.consumed.contains(&event.physical_key) { return true; }
        let toggle = modifiers.alt_key() && matches!(&event.logical_key, Key::Character(text) if text.as_str() == "u");
        if toggle && event.repeat {
            self.consumed.insert(event.physical_key);
            return true;
        }
        if toggle {
            if self.view.is_some() { self.view = None; self.pending = None; }
            else if live.amux_history.columns != 0 {
                self.view = Some(View::new(live)); self.presentation = live.amux_copy_reset;
                self.incarnation = None;
            }
        } else if let Some(view) = self.view.as_mut() {
            let text = match &event.logical_key {
                Key::Character(text) => text.to_string(),
                Key::Named(NamedKey::Space) => " ".into(),
                Key::Named(key) if matches!(key, NamedKey::Escape | NamedKey::Enter | NamedKey::Backspace)
                    || (!view.searching && matches!(key, NamedKey::ArrowLeft | NamedKey::ArrowRight
                        | NamedKey::ArrowUp | NamedKey::ArrowDown | NamedKey::PageUp | NamedKey::PageDown)) => format!("{key:?}"),
                _ => { self.consumed.insert(event.physical_key); return true; },
            };
            if (text == "Escape" && !view.hinting() || (text == "q" && !view.awaiting_character())) && !view.searching { self.view = None; self.pending = None; }
            else { view.key(&text, modifiers.control_key()); }
        } else { return false; }
        self.consumed.insert(event.physical_key);
        true
    }

    pub fn wheel(&mut self, delta: MouseScrollDelta, cell_height: f64, live: &Term<EventProxy>) {
        self.synchronize(live);
        let rows = wheel_rows(delta, cell_height, &mut self.wheel_remainder);
        if rows == 0 || live.amux_history.columns == 0 { return; }
        if self.exit_at.take().is_some() { self.view = None; self.pending = None; }
        if self.view.is_none() && rows > 0 {
            self.view = Some(View::new(live)); self.presentation = live.amux_copy_reset;
            self.incarnation = None;
        }
        self.scrolling = true;
        if let Some(view) = &mut self.view {
            self.pending = None;
            if view.scroll(rows) { self.view = None; self.scrolling = false; }
        }
    }

    pub fn synchronize(&mut self, live: &Term<EventProxy>) {
        if live.amux_history.columns == 0 { return; }
        if self.presentation != live.amux_copy_reset {
            self.scrolling = false;
            self.wheel_remainder = 0.;
        }
        if let Some((view, incarnation)) = self.remembered.take() {
            self.view = Some(view); self.incarnation = incarnation;
            self.presentation = live.amux_copy_reset;
        }
        synchronize_view(&mut self.view, live, &mut self.presentation, &mut self.pending,
            &mut self.focused, &mut self.exit_at);
    }

    pub fn response(&mut self, serial: u64, value: Value) {
        if self.pending != Some(serial) { return; }
        self.pending = None;
        if let Some(incarnation) = value["incarnation"].as_str() {
            if self.incarnation.as_deref().is_some_and(|old| old != incarnation) { self.view = None; return; }
            self.incarnation = Some(incarnation.to_owned());
        }
        if let Some(view) = &mut self.view { view.response(value); }
    }

    pub fn tick(&mut self, live: &Term<EventProxy>, clipboard: &mut Clipboard, scheduler: &mut Scheduler, window_id: WindowId, config: &UiConfig) -> bool {
        self.synchronize(live);
        if let Some(deadline) = self.exit_at {
            if Instant::now() < deadline { return false; }
            self.view = None; self.pending = None; self.exit_at = None;
            return true;
        }
        let Some(view) = &mut self.view else { return false; };
        if !live.is_focused && !self.scrolling { return false; }
        let effect = view.poll();
        let changed = effect.is_some();
        match effect {
            Some(Effect::Read(request)) => {
                self.serial += 1; self.pending = Some(self.serial);
                if self.sender.try_send((self.serial, request)).is_err() {
                    self.pending = None; view.response(json!({"error":"history worker unavailable"}));
                }
            },
            Some(Effect::Clipboard(text, exit)) => {
                if !live.is_focused { view.suspend(); return false; }
                clipboard.store(ClipboardType::Clipboard, text);
                if exit {
                    self.exit_at = Some(Instant::now() + COPY_FEEDBACK);
                    let timer = TimerId::new(Topic::AmuxCopy, window_id);
                    scheduler.unschedule(timer);
                    scheduler.schedule(Event::new(EventType::AmuxContinue, window_id), COPY_FEEDBACK, false, timer);
                }
            },
            Some(Effect::OpenLink(term)) => {
                if let Some((program, args)) = link_command(&term, config) {
                    self.proxy.send_event(EventType::AmuxOpenLink(program, args));
                    view.status = "Opening link".into();
                } else { view.status = "No hyperlink at cursor".into(); }
            },
            Some(Effect::Continue) => self.proxy.send_event(EventType::AmuxContinue),
            None => (),
        }
        if let Some(view) = &self.view { view.notice(); }
        changed
    }
}
