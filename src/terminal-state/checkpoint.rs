use std::io;

use serde::{Deserialize, Serialize};
use vte::ansi::{self, CharsetIndex, CursorStyle, KeyboardModes, Rgb};

use super::{Cell, Event, EventListener, Line, Term, TermMode};
use crate::grid::{Cursor, Dimensions};
use crate::grid::checkpoint::GridState;

#[derive(Serialize, Deserialize)]
pub struct TerminalState {
    history: super::history::Descriptor,
    generation: u64,
    grid: GridState,
    inactive_grid: GridState,
    cursors: [Cursor<Cell>; 4],
    active_charset: CharsetIndex,
    tabs: Vec<bool>,
    mode: u32,
    scroll_region: std::ops::Range<Line>,
    colors: Vec<Option<Rgb>>,
    cursor_style: Option<CursorStyle>,
    title: Option<String>,
    title_stack: Vec<Option<String>>,
    keyboard_mode_stack: Vec<KeyboardModes>,
    inactive_keyboard_mode_stack: Vec<KeyboardModes>,
}

impl TerminalState {
    pub fn capture<T>(term: &Term<T>) -> Self { Self::projection(term, false) }

    fn projection<T>(term: &Term<T>, live: bool) -> Self {
        let grid = |g| if live { GridState::screen(g) } else { GridState::capture(g) };
        Self {
            history: term.history_descriptor(), generation: term.amux_generation,
            grid: grid(&term.grid),
            inactive_grid: grid(&term.inactive_grid),
            cursors: [term.grid.cursor.clone(), term.grid.saved_cursor.clone(),
                      term.inactive_grid.cursor.clone(), term.inactive_grid.saved_cursor.clone()],
            active_charset: term.active_charset, tabs: term.tabs.tabs.clone(),
            mode: term.mode.bits(), scroll_region: term.scroll_region.clone(),
            colors: (0..super::color::COUNT).map(|i| term.colors[i]).collect(),
            cursor_style: term.cursor_style, title: term.title.clone(),
            title_stack: term.title_stack.clone(),
            keyboard_mode_stack: term.keyboard_mode_stack.clone(),
            inactive_keyboard_mode_stack: term.inactive_keyboard_mode_stack.clone(),
        }
    }

    pub fn restore<T: EventListener>(self, term: &mut Term<T>) -> io::Result<()> {
        if self.colors.len() != super::color::COUNT {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "invalid color checkpoint"));
        }
        let grid = self.grid.restore()?;
        let inactive_grid = self.inactive_grid.restore()?;
        term.amux_history = self.history;
        term.amux_generation = self.generation;
        term.grid = grid;
        term.inactive_grid = inactive_grid;
        let [cursor, saved, inactive_cursor, inactive_saved] = self.cursors;
        term.grid.cursor = cursor;
        term.grid.saved_cursor = saved;
        term.inactive_grid.cursor = inactive_cursor;
        term.inactive_grid.saved_cursor = inactive_saved;
        term.active_charset = self.active_charset;
        term.tabs.tabs = self.tabs;
        term.mode = TermMode::from_bits_retain(self.mode);
        term.scroll_region = self.scroll_region;
        for (index, color) in self.colors.into_iter().enumerate() { term.colors[index] = color; }
        term.cursor_style = self.cursor_style;
        term.title = self.title;
        term.title_stack = self.title_stack;
        term.keyboard_mode_stack = self.keyboard_mode_stack;
        term.inactive_keyboard_mode_stack = self.inactive_keyboard_mode_stack;
        term.selection = None;
        term.vi_mode_cursor = Default::default();
        term.damage.resize(term.columns(), term.screen_lines());
        term.event_proxy.send_event(match &term.title {
            Some(title) => Event::Title(title.clone()), None => Event::ResetTitle,
        });
        term.event_proxy.send_event(Event::CursorBlinkingChange);
        Ok(())
    }
}

#[derive(Deserialize)]
pub struct Checkpoint {
    pub term: TerminalState,
    pub parser: ansi::Processor,
}

pub fn capture<T>(term: &Term<T>, parser: &ansi::Processor) -> io::Result<Vec<u8>> {
    #[derive(Serialize)]
    struct Snapshot<'a> { term: TerminalState, parser: &'a ansi::Processor }
    serde_json::to_vec(&Snapshot { term: TerminalState::capture(term), parser })
        .map_err(io::Error::other)
}

pub fn capture_live<T>(term: &Term<T>, parser: &ansi::Processor) -> io::Result<Vec<u8>> {
    #[derive(Serialize)]
    struct Snapshot<'a> { term: TerminalState, parser: &'a ansi::Processor }
    serde_json::to_vec(&Snapshot { term: TerminalState::projection(term, true), parser }).map_err(io::Error::other)
}

pub fn restore<T: EventListener>(term: &mut Term<T>, parser: &mut ansi::Processor, data: &[u8]) -> io::Result<()> {
    let checkpoint: Checkpoint = serde_json::from_slice(data).map_err(io::Error::other)?;
    checkpoint.term.restore(term)?;
    *parser = checkpoint.parser;
    Ok(())
}

pub struct Size(pub usize, pub usize);
impl Dimensions for Size {
    fn columns(&self) -> usize { self.0 }
    fn screen_lines(&self) -> usize { self.1 }
    fn total_lines(&self) -> usize { self.1 }
}

#[derive(Default)]
pub struct Frames { buffer: Vec<u8> }

impl Frames {
    pub fn advance<T: EventListener>(&mut self, term: &mut Term<T>, parser: &mut ansi::Processor, data: &[u8]) -> io::Result<()> {
        self.buffer.extend_from_slice(data);
        let mut offset = 0;
        while self.buffer.len() - offset >= 5 {
            let kind = self.buffer[offset];
            let length = u32::from_be_bytes(self.buffer[offset + 1..offset + 5].try_into().unwrap()) as usize;
            if self.buffer.len() - offset - 5 < length { break; }
            let payload = &self.buffer[offset + 5..offset + 5 + length];
            match kind {
                b'U' => {
                    if payload.len() < 4 { return Err(io::Error::other("short live update")); }
                    let len = u32::from_be_bytes(payload[..4].try_into().unwrap()) as usize;
                    if len > payload.len() - 4 { return Err(io::Error::other("short live descriptor")); }
                    let descriptor = serde_json::from_slice(&payload[4..4 + len]).map_err(io::Error::other)?;
                    parser.advance(term, &payload[4 + len..]);
                    term.amux_history = descriptor;
                },
                b'P' => { term.amux_copy_reset += 1; },
                b'C' => restore(term, parser, payload)?,
                _ => return Err(io::Error::new(io::ErrorKind::InvalidData, "unknown terminal frame")),
            }
            offset += 5 + length;
        }
        self.buffer.drain(..offset);
        Ok(())
    }
}

