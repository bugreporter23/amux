//! Client-local frozen view over the native history projections.
use std::collections::BTreeMap;
use std::sync::Arc;

use serde_json::{Value, json};

use crate::event::VoidListener;
use crate::index::{Column, Line, Point, Side, Direction};
use crate::selection::{Selection, SelectionType};
use crate::sync::FairMutex;
use crate::term::Term;
use crate::term::checkpoint::{Size, TerminalState};
use crate::term::cell::{Cell, Flags};
use crate::term::history::{Descriptor, Row, MAX_CELLS, RowText};
use crate::term::search::RegexSearch;
use crate::vi_mode::ViMotion;
use crate::vte::ansi::{CursorShape, CursorStyle};

const CACHE_BYTES: usize = 4 * 1024 * 1024;
const COPY_BYTES: usize = 8 * 1024 * 1024;

type Position = (u64, usize);

pub enum Effect {
    Read(Value),
    Clipboard(String, bool),
    OpenLink(Box<Term<VoidListener>>),
    Continue,
}

struct Cached { row: Row, styled: bool, bytes: usize, used: u64 }
struct MotionJob { motion: ViMotion, first: u64, end: u64, remaining: usize }
enum LineAction { Copy, Object(String, bool), OpenLink }
struct LineJob { first: u64, last: u64, backward: bool, action: LineAction }
struct CopyJob { start: Position, end: Position, next: u64, block: bool, exit: bool, validated: bool, text: String, extractor: RowText }
struct SearchJob { regex: RegexSearch, next: u64, rows: Vec<Row>, bytes: usize, first: u64, candidate: Option<Position>, fallback: Option<Position> }
struct MarkerJob { origin: Position, next: u64, direction: i8, marker: u8, action: MarkerAction }
#[derive(Clone, Copy)]
enum MarkerAction { Jump, Select(bool), Command }
enum RowCommand { Find(char, char, usize, bool) }

enum Pending { Rows { start: u64, end: u64, styled: bool, generation: u64 }, Validate(u64), Marker(u64) }

pub struct View {
    pub terminal: Arc<FairMutex<Term<VoidListener>>>,
    pub cut: Descriptor,
    pub status: String,
    pub cursor: Position,
    pub searching: bool,
    tail: Vec<Row>,
    cache: BTreeMap<u64, Cached>,
    bytes: usize,
    clock: u64,
    floor: u64,
    top: u64,
    selection: Option<(Position, bool)>,
    pending: Option<Pending>,
    copy: Option<CopyJob>,
    marker: Option<MarkerJob>,
    search: Option<SearchJob>,
    query: String,
    direction: i8,
    query_direction: i8,
    search_skip: bool,
    search_origin: Position,
    prefix: char,
    count: Option<usize>,
    command_count: usize,
    last_find: Option<(char, char)>,
    object: Option<bool>,
    row_command: Option<RowCommand>,
    motion: Option<MotionJob>,
    line: Option<LineJob>,
    command: bool,
    generation: u64,
    invalid: bool,
    read_error: bool,
    eager: bool,
    waiting: bool,
    finish_copy: Option<bool>,
}

impl View {
    pub fn new<T>(live: &Term<T>) -> Self {
        let cut = live.amux_history.clone();
        let tail = live.screen_rows();
        let mut terminal = Term::new(live.config.clone(), &Size(cut.columns, cut.lines), VoidListener);
        TerminalState::capture(live).restore(&mut terminal).expect("native live snapshot");
        terminal.config.vi_mode_cursor_style = Some(CursorStyle { shape: CursorShape::Block, blinking: false });
        terminal.toggle_vi_mode();
        let cursor = (cut.next + live.grid().cursor.point.line.0.max(0) as u64, live.grid().cursor.point.column.0);
        let floor = if cut.alternate { cut.next } else { cut.oldest };
        let mut view = Self { terminal: Arc::new(FairMutex::new(terminal)), top: cut.next,
            cut, tail, floor, cursor, selection: None, cache: BTreeMap::new(), bytes: 0, clock: 0,
            status: String::new(), pending: None, copy: None, marker: None, search: None,
            query: String::new(), direction: 1, query_direction: 1, search_skip: false, search_origin: cursor, searching: false,
            prefix: '\0', count: None, command_count: 1, last_find: None, object: None, row_command: None, motion: None, line: None, command: false, generation: 0, invalid: false, read_error: false, eager: false, waiting: false, finish_copy: None };
        view.render();
        view
    }

    pub fn blur(&mut self) {
        self.copy = None; self.marker = None; self.line = None; self.motion = None; self.row_command = None;
        self.count = None; self.prefix = '\0'; self.object = None; self.finish_copy = None;
        self.generation += 1;
    }

    pub fn valid(&self, descriptor: &Descriptor) -> bool {
        !self.invalid && self.cut.epoch == descriptor.epoch && self.cut.columns == descriptor.columns
            && self.cut.lines == descriptor.lines && self.cut.alternate == descriptor.alternate
    }

    pub fn update(&mut self, descriptor: &Descriptor) {
        self.floor = if self.cut.alternate { self.cut.next } else { descriptor.oldest.min(self.cut.next) };
        let expired: Vec<_> = self.cache.range(..self.floor).map(|(&id, _)| id).collect();
        for id in expired { self.bytes -= self.cache.remove(&id).unwrap().bytes; }
        if self.cursor.0 < self.floor {
            self.cursor.0 = self.floor;
            self.status = "History evicted; moved to oldest row".into();
        }
        if self.selection.is_some_and(|(anchor, _)| anchor.0 < self.floor) || self.copy.as_ref().is_some_and(|job| job.start.0 < self.floor)
            || self.line.as_ref().is_some_and(|job| job.first < self.floor) {
            self.selection = None; self.copy = None; self.line = None;
            self.status = "Selection was evicted".into();
        }
    }

    fn last(&self) -> u64 { self.cut.next + self.tail.len() as u64 - 1 }
    pub fn line_indicator(&self) -> (u64, u64) { (self.last().saturating_sub(self.cursor.0), self.last() - self.floor) }
    fn page(&self) -> u64 { (self.cut.lines * 2).min(128).min(MAX_CELLS / self.cut.columns).max(1) as u64 }
    fn copy_page(&self) -> u64 { 128.min(MAX_CELLS / self.cut.columns).max(1) as u64 }
    fn row(&self, id: u64, styled: bool) -> Option<&Row> {
        if id >= self.cut.next { return self.tail.get((id - self.cut.next) as usize); }
        self.cache.get(&id).filter(|entry| !styled || entry.styled).map(|entry| &entry.row)
    }
    fn request_rows(&mut self, start: u64, end: u64, styled: bool, whole: Option<[u64; 2]>) -> Effect {
        if self.eager { self.waiting = true; return Effect::Continue; }
        self.pending = Some(Pending::Rows { start, end, styled, generation: self.generation });
        self.status = if self.search.is_some() { format!("Search: {} · reading history", self.query) } else { "Reading history…".into() };
        Effect::Read(json!({"kind":"rows", "epoch":self.cut.epoch,"start":start,"end":end,
            "projection":if styled { "cells" } else { "text" },"whole":whole}))
    }
    fn ensure(&mut self, start: u64, end: u64, styled: bool, whole: Option<[u64; 2]>) -> Option<Effect> {
        let end = end.min(self.cut.next);
        let missing = (start..end).find(|id| self.row(*id, styled).is_none())?;
        let page = if whole.is_some() { self.copy_page() } else { self.page() };
        Some(self.request_rows(missing, (missing + page).min(if styled && whole.is_none() { self.cut.next } else { end }), styled, whole))
    }

    pub fn response(&mut self, value: Value) {
        let Some(pending) = self.pending.take() else { return; };
        if let Some(error) = value.get("error").and_then(Value::as_str) {
            let generation = match &pending { Pending::Rows { generation, .. } | Pending::Validate(generation) | Pending::Marker(generation) => *generation };
            if generation != self.generation { return; }
            self.read_error = true;
            self.status = error.into(); self.copy = None; self.marker = None; self.search = None; self.line = None;
            self.motion = None; self.finish_copy = None;
            self.render(); return;
        }
        let Ok(descriptor) = serde_json::from_value::<Descriptor>(value["descriptor"].clone()) else {
            self.status = "Invalid history response".into(); return;
        };
        if !self.valid(&descriptor) { self.invalid = true; self.status = "History epoch changed".into(); return; }
        self.update(&descriptor);
        match pending {
            Pending::Rows { start, end, styled, .. } => {
                if value["start"].as_u64() != Some(start) { self.status = "History page identity mismatch".into(); return; }
                let Ok(rows) = serde_json::from_value::<Vec<Row>>(value["rows"].clone()) else { self.status = "Invalid history page".into(); return; };
                if rows.len() as u64 != end - start { self.invalid = true; self.status = "Incomplete history page".into(); return; }
                self.clock += 1;
                for (offset, row) in rows.into_iter().enumerate() {
                    if row.cells.len() != self.cut.columns { self.status = "History row geometry mismatch".into(); return; }
                    let id = start + offset as u64;
                    if id < self.floor { continue; }
                    if self.cache.get(&id).is_some_and(|entry| entry.styled && !styled) { continue; }
                    let bytes = row.resident_bytes();
                    if let Some(old) = self.cache.insert(id, Cached { row, styled, bytes, used: self.clock }) { self.bytes -= old.bytes; }
                    self.bytes += bytes;
                }
                self.evict();
            },
            Pending::Validate(generation) if generation == self.generation => if let Some(job) = &mut self.copy { job.validated = true; },
            Pending::Marker(generation) if generation == self.generation => {
                if let Some(anchor) = value.get("anchor").filter(|a| !a.is_null()) {
                    if let Ok([row, col]) = serde_json::from_value::<[u64; 2]>(anchor.clone()) { self.marker_found((row, col as usize)); }
                }
            },
            _ => (),
        }
    }

    fn evict(&mut self) {
        while self.bytes > CACHE_BYTES {
            let mut candidates: Vec<_> = self.cache.iter().filter(|(id, _)| (**id < self.top || **id >= self.top + self.cut.lines as u64)
                && !self.motion.as_ref().is_some_and(|job| **id >= job.first && **id < job.end)
                && !self.line.as_ref().is_some_and(|job| matches!(job.action, LineAction::Object(..) | LineAction::OpenLink)
                    && **id >= job.first.saturating_sub(self.page()) && **id <= job.last.saturating_add(self.page())))
                .map(|(&id, entry)| (entry.used, id)).collect();
            if candidates.is_empty() {
                if self.motion.take().is_some() { self.status = "Motion exceeds history working budget".into(); continue; }
                if self.line.as_ref().is_some_and(|job| matches!(job.action, LineAction::Object(..) | LineAction::OpenLink)) {
                    self.line = None; self.status = "Text object exceeds history working budget".into(); continue;
                }
                break;
            }
            candidates.sort_unstable();
            for (_, id) in candidates {
                self.bytes -= self.cache.remove(&id).unwrap().bytes;
                if self.bytes <= CACHE_BYTES { break; }
            }
        }
    }

    fn render(&mut self) -> bool {
        if self.cursor.0 < self.top { self.top = self.cursor.0; }
        if self.cursor.0 >= self.top + self.cut.lines as u64 { self.top = self.cursor.0 - self.cut.lines as u64 + 1; }
        self.top = self.top.max(self.floor).min(self.last().saturating_sub(self.cut.lines as u64 - 1).max(self.floor));
        let end = self.top + self.cut.lines as u64;
        if (self.top..end).any(|id| self.row(id, true).is_none()) { return false; }
        let mut term = self.terminal.lock();
        for (line, id) in (self.top..end).enumerate() {
            for (col, cell) in self.row(id, true).unwrap().cells.iter().enumerate() {
                term.grid_mut()[Line(line as i32)][Column(col)] = cell.clone();
            }
        }
        term.vi_mode_cursor.point = Point::new(Line((self.cursor.0 - self.top) as i32), Column(self.cursor.1));
        term.selection = self.selection.and_then(|(anchor, block)| {
            let (a, b) = if anchor <= self.cursor { (anchor, self.cursor) } else { (self.cursor, anchor) };
            if b.0 < self.top || a.0 >= end { return None; }
            let start = Point::new(Line(a.0.saturating_sub(self.top) as i32), Column(if a.0 < self.top && !block { 0 } else { a.1 }));
            let finish = Point::new(Line((b.0.min(end - 1) - self.top) as i32), Column(if b.0 >= end && !block { self.cut.columns - 1 } else { b.1 }));
            let mut selection = Selection::new(if block { SelectionType::Block } else { SelectionType::Simple }, start, Side::Left);
            selection.update(finish, Side::Right);
            Some(selection)
        });
        term.mark_fully_damaged();
        true
    }

    pub fn notice(&self) {
        if self.status.is_empty() { return; }
        let mut term = self.terminal.lock();
        let line = Line(self.cut.lines as i32 - 1);
        for col in 0..self.cut.columns { term.grid_mut()[line][Column(col)] = Cell::default(); }
        for (col, c) in self.status.chars().take(self.cut.columns).enumerate() { term.grid_mut()[line][Column(col)].c = c; }
        term.mark_fully_damaged();
    }

    pub fn awaiting_character(&self) -> bool {
        self.object.is_some() || matches!(self.prefix, 'f' | 'F' | 't' | 'T')
    }

    pub fn key(&mut self, key: &str, control: bool) {
        if !control && matches!(key, "y" | "Y") && (self.motion.is_some()
            || self.line.as_ref().is_some_and(|job| matches!(job.action, LineAction::Object(..)))) {
            self.finish_copy = Some(key == "y"); return;
        }
        self.finish_copy = None;
        self.read_error = false;
        self.generation += 1;
        self.status.clear();
        if self.searching {
            match key {
                "Enter" => { self.searching = false; self.status.clear(); },
                "Escape" => { self.searching = false; self.search = None; self.cursor = self.search_origin; },
                "Backspace" => { self.query.pop(); self.start_search(); },
                _ if !control && key.chars().all(|c| !c.is_control()) => { self.query.push_str(key); self.start_search(); },
                _ => (),
            }
            return;
        }
        self.copy = None; self.marker = None; self.search = None; self.command = false; self.motion = None; self.line = None; self.row_command = None;
        if control && key.eq_ignore_ascii_case("k") {
            self.prefix = '\0'; self.count = None; self.object = None;
            self.line = Some(LineJob { first: self.cursor.0, last: self.cursor.0, backward: true, action: LineAction::OpenLink });
            return;
        }
        if let Some(around) = self.object.take() {
            if !control { self.line = Some(LineJob { first: self.cursor.0, last: self.cursor.0,
                backward: true, action: LineAction::Object(key.into(), around) }); }
            self.settle_selection();
            return;
        }
        if matches!(self.prefix, 'f' | 'F' | 't' | 'T') {
            let motion = std::mem::replace(&mut self.prefix, '\0');
            if !control && key.chars().count() == 1 {
                let target = key.chars().next().unwrap();
                self.last_find = Some((motion, target));
                self.row_command = Some(RowCommand::Find(motion, target, self.command_count, false));
            }
            self.apply_row_command();
            return;
        }
        if self.prefix == 'g' { self.prefix = '\0'; if key == "g" {
            self.cursor.0 = self.floor.saturating_add((self.command_count - 1) as u64).min(self.last()); return;
        } }
        if !control && key.len() == 1 && key.as_bytes()[0].is_ascii_digit() && (key != "0" || self.count.is_some()) {
            let digit = usize::from(key.as_bytes()[0] - b'0');
            self.count = self.count.unwrap_or(0).checked_mul(10).and_then(|n| n.checked_add(digit));
            if self.count.is_none() { self.status = "Motion count is too large".into(); }
            return;
        }
        let specified = self.count.take();
        let count = specified.unwrap_or(1);
        if specified.is_some() && !(if control { matches!(key.to_lowercase().as_str(), "u" | "b" | "d" | "f") }
            else { matches!(key, "j" | "k" | "J" | "K" | "ArrowDown" | "ArrowUp" | "PageUp" | "PageDown"
                | "g" | "G" | "f" | "F" | "t" | "T" | ";" | "," | "h" | "l" | "ArrowLeft" | "ArrowRight"
                | "0" | "$" | "^" | "w" | "b" | "e" | "W" | "B" | "E") }) {
            self.status = "Counts are supported for navigation and character searches".into(); return;
        }
        if control {
            match key.to_lowercase().as_str() {
                "v" => self.selection = Some((self.selection.map_or(self.cursor, |(a, _)| a), !self.selection.is_some_and(|(_, b)| b))),
                "u" | "b" => self.cursor.0 = self.cursor.0.saturating_sub((if key == "u" { self.cut.lines as u64 / 2 } else { self.cut.lines as u64 }).saturating_mul(count as u64)).max(self.floor),
                "d" | "f" => self.cursor.0 = self.cursor.0.saturating_add((if key == "d" { self.cut.lines as u64 / 2 } else { self.cut.lines as u64 }).saturating_mul(count as u64)).min(self.last()),
                _ => (),
            }
            return;
        }
        match key {
            "j" | "ArrowDown" | "J" => self.cursor.0 = self.cursor.0.saturating_add((if key == "J" { 10u64 } else { 1 }).saturating_mul(count as u64)).min(self.last()),
            "k" | "ArrowUp" | "K" => self.cursor.0 = self.cursor.0.saturating_sub((if key == "K" { 10u64 } else { 1 }).saturating_mul(count as u64)).max(self.floor),
            "PageUp" => self.cursor.0 = self.cursor.0.saturating_sub((self.cut.lines as u64).saturating_mul(count as u64)).max(self.floor),
            "PageDown" => self.cursor.0 = self.cursor.0.saturating_add((self.cut.lines as u64).saturating_mul(count as u64)).min(self.last()),
            "g" => { self.prefix = 'g'; self.command_count = count; },
            "G" => self.cursor.0 = specified.map_or(self.last(), |n| self.floor.saturating_add((n - 1) as u64).min(self.last())),
            "f" | "F" | "t" | "T" => { self.prefix = key.chars().next().unwrap(); self.command_count = count; },
            ";" | "," => {
                if let Some((mut motion, target)) = self.last_find {
                    if key == "," { motion = match motion { 'f' => 'F', 'F' => 'f', 't' => 'T', _ => 't' }; }
                    self.row_command = Some(RowCommand::Find(motion, target, count, true));
                    self.apply_row_command();
                } else { self.status = "No character search to repeat".into(); }
            },
            "v" => self.selection = if self.selection.is_some() { None } else { Some((self.cursor, false)) },
            "V" => { self.cursor.1 = self.cut.columns - 1; self.selection = Some(((self.cursor.0, 0), false)); },
            "i" | "a" if self.selection.is_some() => self.object = Some(key == "a"),
            "/" | "?" => { self.searching = true; self.query.clear(); self.query_direction = if key == "/" { 1 } else { -1 }; self.direction = self.query_direction; self.search_skip = false; self.search_origin = self.cursor; },
            "n" | "N" => { self.direction = if key == "N" { -self.query_direction } else { self.query_direction }; self.search_skip = true; self.search_origin = self.cursor; self.start_search(); },
            "p" | "P" => self.start_marker(b'A', if key == "p" { -1 } else { 1 }, MarkerAction::Jump),
            "c" => self.start_marker(b'A', -1, MarkerAction::Command),
            "y" | "Y" => {
                if self.selection.is_some() { self.start_copy(key == "y"); }
                else { self.start_marker(b'C', 1, MarkerAction::Select(key == "y")); }
            },
            "L" => self.line = Some(LineJob { first: self.cursor.0, last: self.cursor.0, backward: true, action: LineAction::Copy }),
            key => {
                let motion = match key {
                    "h" | "ArrowLeft" => Some(ViMotion::Left), "l" | "ArrowRight" => Some(ViMotion::Right),
                    "0" => Some(ViMotion::First), "$" => Some(ViMotion::Last), "^" => Some(ViMotion::FirstOccupied),
                    "w" => Some(ViMotion::SemanticRight), "b" => Some(ViMotion::SemanticLeft), "e" => Some(ViMotion::SemanticRightEnd),
                    "W" => Some(ViMotion::WordRight), "B" => Some(ViMotion::WordLeft), "E" => Some(ViMotion::WordRightEnd), "%" => Some(ViMotion::Bracket),
                    _ => None,
                };
                self.motion = motion.map(|motion| MotionJob { motion, remaining: count,
                    first: if matches!(motion, ViMotion::Bracket) { self.cursor.0.saturating_sub(self.page()).max(self.floor) } else { self.cursor.0 },
                    end: if matches!(motion, ViMotion::Bracket) { (self.cursor.0 + self.page() + 1).min(self.last() + 1) } else { self.cursor.0 + 1 } });
                self.settle_selection();
            },
        }
    }

    fn settle_selection(&mut self) {
        self.eager = true; self.waiting = false;
        for _ in 0..128 {
            if self.pending.is_some() || self.finish_copy.is_some() { break; }
            if self.motion.is_none() && self.line.is_none() { break; }
            if !matches!(self.poll(), Some(Effect::Continue)) || self.waiting { break; }
        }
        self.eager = false;
    }

    fn apply_row_command(&mut self) {
        if self.row(self.cursor.0, true).is_none() { return; }
        if let Some(command) = self.row_command.take() {
            match command {
                RowCommand::Find(motion, target, count, repeat) => self.find_character(motion, target, count, repeat),
            }
        }
    }

    fn find_character(&mut self, motion: char, target: char, count: usize, repeat: bool) {
        let row = self.row(self.cursor.0, true).unwrap();
        let forward = matches!(motion, 'f' | 't');
        let cells: Vec<_> = row.cells.iter().enumerate()
            .filter(|(_, cell)| !cell.flags.intersects(Flags::WIDE_CHAR_SPACER | Flags::LEADING_WIDE_CHAR_SPACER)).collect();
        let at = cells.iter().rposition(|(col, _)| *col <= self.cursor.1).unwrap_or(0);
        let matches = cells.iter().enumerate().filter(|(index, (_, cell))| {
            cell.c == target && if forward {
                *index > at + usize::from(repeat && count == 1 && motion == 't')
            } else { *index + usize::from(repeat && count == 1 && motion == 'T') < at }
        }).map(|(index, _)| index);
        let found = if forward { matches.skip(count - 1).next() } else { matches.rev().skip(count - 1).next() };
        let Some(mut index) = found else { self.status = "Character not found on row".into(); return; };
        if motion == 't' { index = index.saturating_sub(1); }
        if motion == 'T' { index = (index + 1).min(cells.len() - 1); }
        self.cursor.1 = cells[index].0;
    }

    fn text_object(&mut self, first: u64, last: u64, key: &str, around: bool) {
        let cells: Vec<_> = (first..=last).flat_map(|id| self.row(id, false).unwrap().cells.iter().enumerate()
            .filter(|(_, cell)| !cell.flags.intersects(Flags::WIDE_CHAR_SPACER | Flags::LEADING_WIDE_CHAR_SPACER))
            .map(move |(col, cell)| ((id, col), cell))).collect();
        let mut text: Vec<_> = cells.iter().map(|(_, cell)| cell.c).collect();
        let length = text.iter().rposition(|c| *c != ' ').map_or(0, |i| i + 1);
        text.truncate(length);
        let at = cells.iter().rposition(|(position, _)| *position <= self.cursor).unwrap_or(0);
        if at >= length { self.status = "Cursor is outside the row text".into(); return; }
        let (mut a, mut b);
        if matches!(key, "w" | "W") {
            let class = |c: char| if c.is_whitespace() { 0 } else if key == "W" || c.is_alphanumeric() || c == '_' { 1 } else { 2 };
            a = at; b = at;
            while a > 0 && class(text[a - 1]) == class(text[at]) { a -= 1; }
            while b + 1 < text.len() && class(text[b + 1]) == class(text[at]) { b += 1; }
            if around {
                if class(text[at]) == 0 && b + 1 < text.len() {
                    let wanted = class(text[b + 1]);
                    while b + 1 < text.len() && class(text[b + 1]) == wanted { b += 1; }
                } else if b + 1 < text.len() && text[b + 1] == ' ' {
                    while b + 1 < text.len() && text[b + 1] == ' ' { b += 1; }
                } else { while a > 0 && text[a - 1] == ' ' { a -= 1; } }
            }
        } else {
            let (open, close) = match key { "(" | ")" | "b" => ('(', ')'), "[" | "]" => ('[', ']'), "{" | "}" | "B" => ('{', '}'), "<" | ">" => ('<', '>'), "\"" => ('"', '"'), "'" => ('\'', '\''), "`" => ('`', '`'), _ => return };
            let mut stack = Vec::new(); let mut ranges = Vec::new(); let mut backslashes = 0;
            for (i, c) in text.iter().copied().enumerate() {
                if open == close {
                    if c == '\\' { backslashes += 1; continue; }
                    let escaped = backslashes % 2 != 0; backslashes = 0;
                    if c == open && escaped { continue; }
                }
                if c == open && (open != close || stack.is_empty()) { stack.push(i); }
                else if c == close { if let Some(start) = stack.pop() { if start <= at && at <= i { ranges.push((start, i)); } } }
            }
            let Some(range) = ranges.into_iter().min_by_key(|(a, b)| b - a) else { self.status = "No enclosing text object".into(); return; };
            (a, b) = range;
            if !around { a += 1; b = b.saturating_sub(1); }
        }
        if a > b { self.selection = None; return; }
        let start = cells[a].0;
        let (row, col) = cells[b].0;
        let end = (row, col + usize::from(cells[b].1.flags.contains(Flags::WIDE_CHAR)));
        self.selection = Some((start, false)); self.cursor = end;
    }

    fn start_copy(&mut self, exit: bool) {
        let Some((anchor, block)) = self.selection else { return; };
        let (start, end) = if anchor <= self.cursor { (anchor, self.cursor) } else { (self.cursor, anchor) };
        let extractor = RowText::new(self.cut.columns, Some(&self.terminal.lock().tabs.tabs));
        self.copy = Some(CopyJob { start, end, next: start.0, block, exit, validated: start.0 >= self.cut.next, text: String::new(), extractor });
    }
    fn start_marker(&mut self, marker: u8, direction: i8, action: MarkerAction) {
        self.marker = Some(MarkerJob { origin: self.cursor, next: (self.cursor.0 + u64::from(direction < 0)).min(self.cut.next), marker, direction, action });
    }
    fn marker_found(&mut self, anchor: Position) {
        let Some(job) = self.marker.take() else { return; };
        match job.action {
            MarkerAction::Jump => self.cursor = (anchor.0, anchor.1.min(self.cut.columns - 1)),
            MarkerAction::Select(exit) => {
                self.selection = Some((job.origin, false));
                self.cursor = if anchor.1 > 0 { (anchor.0, anchor.1 - 1) } else { (anchor.0.saturating_sub(1).max(self.floor), self.cut.columns - 1) };
                self.start_copy(exit);
            },
            MarkerAction::Command => {
                self.cursor = ((anchor.0 + 1).min(self.last()), 0);
                self.command = true;
            },
        }
    }
    fn start_search(&mut self) {
        if self.query.is_empty() { self.search = None; return; }
        let Ok(regex) = RegexSearch::new(&self.query) else { self.status = "Invalid search regex".into(); self.search = None; return; };
        self.search = Some(SearchJob { regex, next: self.floor, first: self.floor, rows: Vec::new(), bytes: 0, candidate: None, fallback: None });
    }

    pub fn poll(&mut self) -> Option<Effect> {
        if self.pending.is_some() || self.read_error { return None; }
        if self.cursor.0 < self.top { self.top = self.cursor.0; }
        if self.cursor.0 >= self.top + self.cut.lines as u64 { self.top = self.cursor.0 - self.cut.lines as u64 + 1; }
        self.top = self.top.max(self.floor).min(self.last().saturating_sub(self.cut.lines as u64 - 1).max(self.floor));
        if let Some(effect) = self.ensure(self.top, self.top + self.cut.lines as u64, true, None) { return Some(effect); }
        self.apply_row_command();
        if let Some(mut job) = self.line.take() {
            for _ in 0..self.page() {
                if job.backward && job.first > self.floor {
                    let previous = job.first - 1;
                    let Some(row) = self.row(previous, false) else {
                        let start = job.first.saturating_sub(self.page()).max(self.floor);
                        let end = job.first;
                        self.line = Some(job);
                        return Some(self.request_rows(start, end, false, None));
                    };
                    if row.cells.last().unwrap().flags.contains(Flags::WRAPLINE) {
                        job.first = previous;
                        continue;
                    }
                }
                job.backward = false;
                let Some(row) = self.row(job.last, false) else {
                    let start = job.last;
                    let end = (start + self.page()).min(self.cut.next);
                    self.line = Some(job);
                    return Some(self.request_rows(start, end, false, None));
                };
                if job.last < self.last() && row.cells.last().unwrap().flags.contains(Flags::WRAPLINE) {
                    job.last += 1;
                    continue;
                }
                match &job.action {
                    LineAction::OpenLink => {
                        if let Some(effect) = self.ensure(job.first, job.last + 1, true, None) {
                            self.line = Some(job); return Some(effect);
                        }
                        let bytes: usize = (job.first..=job.last).map(|id| self.row(id, true).unwrap().resident_bytes()).sum();
                        if bytes > CACHE_BYTES { self.status = "Link exceeds history working budget".into(); return None; }
                        let mut term = Term::new(self.terminal.lock().config.clone(), &Size(self.cut.columns, (job.last - job.first + 1) as usize), VoidListener);
                        for (line, id) in (job.first..=job.last).enumerate() {
                            for (col, cell) in self.row(id, true).unwrap().cells.iter().enumerate() { term.grid_mut()[Line(line as i32)][Column(col)] = cell.clone(); }
                        }
                        term.vi_mode_cursor.point = Point::new(Line((self.cursor.0 - job.first) as i32), Column(self.cursor.1));
                        self.render();
                        return Some(Effect::OpenLink(Box::new(term)));
                    },
                    LineAction::Copy => {
                        self.selection = Some(((job.first, 0), false));
                        self.cursor = (job.last, self.cut.columns - 1);
                        self.start_copy(true);
                    },
                    LineAction::Object(key, around) => {
                        if let Some(effect) = self.ensure(job.first, job.last + 1, false, None) {
                            self.line = Some(job); return Some(effect);
                        }
                        let bytes: usize = (job.first..=job.last).map(|id| self.row(id, false).unwrap().resident_bytes()).sum();
                        if bytes > CACHE_BYTES { self.status = "Text object exceeds history working budget".into(); return None; }
                        self.text_object(job.first, job.last, key, *around);
                        self.render();
                    },
                }
                return Some(Effect::Continue);
            }
            self.line = Some(job);
            return Some(Effect::Continue);
        }
        if let Some(mut job) = self.motion.take() {
            if let Some(effect) = self.ensure(job.first, job.end, true, None) { self.motion = Some(job); return Some(effect); }
            let mut term = Term::new(self.terminal.lock().config.clone(), &Size(self.cut.columns, (job.end - job.first) as usize), VoidListener);
            for (line, id) in (job.first..job.end).enumerate() {
                for (col, cell) in self.row(id, true).unwrap().cells.iter().enumerate() { term.grid_mut()[Line(line as i32)][Column(col)] = cell.clone(); }
            }
            term.toggle_vi_mode();
            term.vi_mode_cursor.point = Point::new(Line((self.cursor.0 - job.first) as i32), Column(self.cursor.1));
            for _ in 0..self.page() {
                term.vi_motion(job.motion);
                let point = term.vi_mode_cursor.point;
                let anchor = (job.first + point.line.0 as u64, point.column.0);
                let left = matches!(job.motion, ViMotion::Left | ViMotion::First | ViMotion::FirstOccupied | ViMotion::SemanticLeft | ViMotion::WordLeft);
                let right = matches!(job.motion, ViMotion::Right | ViMotion::Last | ViMotion::SemanticRight | ViMotion::SemanticRightEnd | ViMotion::WordRight | ViMotion::WordRightEnd);
                if left && anchor.0 == job.first && anchor.1 == 0 && job.first > self.floor {
                    job.first = job.first.saturating_sub(self.page()).max(self.floor);
                    self.motion = Some(job); return Some(Effect::Continue);
                }
                if right && anchor.0 == job.end - 1 && anchor.1 == self.cut.columns - 1 && job.end <= self.last() {
                    job.end = (job.end + self.page()).min(self.last() + 1);
                    self.motion = Some(job); return Some(Effect::Continue);
                }
                job.remaining -= 1;
                let moved = self.cursor != anchor;
                self.cursor = anchor;
                if !moved { job.remaining = 0; }
                if job.remaining == 0 { break; }
            }
            if job.remaining > 0 {
                self.motion = Some(job);
                self.render();
                return Some(Effect::Continue);
            }
        }

        if let Some(exit) = self.finish_copy.take() {
            if self.status.is_empty() || self.status == "Reading history…" {
                if self.selection.is_some() { self.start_copy(exit); }
                else { self.start_marker(b'C', 1, MarkerAction::Select(exit)); }
            }
        }
        if self.command {
            self.command = false;
            self.cursor.1 = self.row(self.cursor.0, true).unwrap().cells.iter().position(|cell| cell.c != ' ').unwrap_or(0);
            self.start_marker(b'C', 1, MarkerAction::Select(true));
        }
        if let Some(mut job) = self.copy.take() {
            if !job.validated {
                let whole = [job.start.0, (job.end.0 + 1).min(self.cut.next)];
                self.pending = Some(Pending::Validate(self.generation)); self.copy = Some(job);
                return Some(Effect::Read(json!({"kind":"rows","epoch":self.cut.epoch,"start":whole[0],"end":whole[0],"projection":"text","whole":whole})));
            }
            let whole = if job.start.0 < self.cut.next { Some([job.start.0, (job.end.0 + 1).min(self.cut.next)]) } else { None };
            let batch = self.copy_page().saturating_sub(1).max(1);
            if let Some(effect) = self.ensure(job.next, (job.next + batch + 1).min(job.end.0 + 2), false, whole) { self.copy = Some(job); return Some(effect); }
            let end = (job.next + batch).min(job.end.0 + 1);
            for id in job.next..end {
                let a = if job.block { job.start.1.min(job.end.1) } else if id == job.start.0 { job.start.1 } else { 0 };
                let b = if job.block { job.start.1.max(job.end.1) } else if id == job.end.0 { job.end.1 } else { self.cut.columns - 1 };
                let text = job.extractor.extract(self.row(id, false).unwrap(), self.row(id + 1, false), a, b,
                    id == job.end.0 || (job.block && a != 0));
                if job.block { job.text.push_str(text.trim_end()); if id < job.end.0 { job.text.push('\n'); } }
                else { job.text.push_str(&text); }
            }
            job.next = end;
            if job.text.len() > COPY_BYTES { self.status = "Copy exceeds the 8 MiB clipboard budget".into(); return None; }
            if job.next > job.end.0 {
                self.status.clear();
                if !job.exit { self.selection = None; }
                self.render();
                if job.text.ends_with('\n') { job.text.pop(); }
                return Some(Effect::Clipboard(job.text, job.exit));
            }
            self.copy = Some(job); return Some(Effect::Continue);
        }
        if let Some(mut job) = self.marker.take() {
            if job.direction < 0 && job.next >= self.cut.next || job.direction > 0 && job.next >= self.cut.next {
                let bound = self.cut.next; let marker = job.marker;
                let mut anchors = self.tail.iter().enumerate().flat_map(|(line, row)| row.cells.iter().enumerate().flat_map(move |(col, cell)| cell.amux_markers().iter().filter_map(move |anchor| if anchor.kind == marker { Some((bound + line as u64, col + usize::from(anchor.after))) } else { None })));
                let found = if job.direction < 0 { anchors.filter(|a| *a < job.origin).last() } else { anchors.find(|a| *a > job.origin) };
                if let Some(anchor) = found { self.marker = Some(job); self.marker_found(anchor); return Some(Effect::Continue); }
                if job.direction > 0 { self.status = "No next prompt marker".into(); return None; }
            }
            let (start, end) = if job.direction < 0 { (job.next.saturating_sub(self.page()).max(self.floor), job.next.min(self.cut.next)) } else { (job.next.max(self.floor), (job.next + self.page()).min(self.cut.next)) };
            if start >= end {
                if job.direction > 0 && job.next < self.cut.next { job.next = self.cut.next; self.marker = Some(job); return Some(Effect::Continue); }
                self.status = "No prompt marker".into(); return None;
            }
            let message = json!({"kind":"marker","epoch":self.cut.epoch,"bound":self.cut.next,"start":start,"end":end,
                "origin":[job.origin.0,job.origin.1],"direction":job.direction,"marker":job.marker});
            job.next = if job.direction < 0 { start } else { end };
            self.marker = Some(job); self.pending = Some(Pending::Marker(self.generation));
            return Some(Effect::Read(message));
        }
        if let Some(mut job) = self.search.take() {
            let end = (job.next + self.page()).min(self.last() + 1);
            if let Some(effect) = self.ensure(job.next, end, false, None) { self.search = Some(job); return Some(effect); }
            for id in job.next..end {
                let row = self.row(id, false).unwrap();
                let wrap = row.cells.last().unwrap().flags.contains(Flags::WRAPLINE);
                job.bytes += row.resident_bytes();
                job.rows.push(row.clone());
                if job.bytes > CACHE_BYTES {
                    self.status = "Wrapped search line exceeds the 4 MiB working budget".into(); return None;
                }
                if !wrap || id == self.last() {
                    let mut term = Term::new(Default::default(), &Size(self.cut.columns, job.rows.len().max(1)), VoidListener);
                    for (line, row) in job.rows.iter().enumerate() { for (col, cell) in row.cells.iter().enumerate() { term.grid_mut()[Line(line as i32)][Column(col)] = cell.clone(); } }
                    let mut origin = if self.search_origin.0 >= job.first && self.search_origin.0 <= id {
                        Point::new(Line((self.search_origin.0 - job.first) as i32), Column(self.search_origin.1))
                    } else if self.direction > 0 { Point::new(Line(0), Column(0)) } else { Point::new(Line(job.rows.len() as i32 - 1), Column(self.cut.columns - 1)) };
                    if self.search_skip && self.search_origin.0 >= job.first && self.search_origin.0 <= id {
                        use crate::index::Boundary;
                        origin = if self.direction > 0 { origin.add(&term, Boundary::Grid, 1) } else { origin.sub(&term, Boundary::Grid, 1) };
                    }
                    if let Some(found) = term.search_next(&mut job.regex, origin, if self.direction > 0 { Direction::Right } else { Direction::Left }, Side::Left, None) {
                        let point = *found.start(); let anchor = (job.first + point.line.0 as u64, point.column.0);
                        if job.fallback.is_none() || self.direction < 0 { job.fallback = Some(anchor); }
                        if self.direction > 0 && (anchor > self.search_origin || (!self.search_skip && anchor == self.search_origin)) { job.candidate = Some(anchor); }
                        if self.direction < 0 && (anchor < self.search_origin || (!self.search_skip && anchor == self.search_origin)) { job.candidate = Some(anchor); }
                    }
                    job.rows.clear(); job.bytes = 0; job.first = id + 1;
                    if self.direction > 0 && job.candidate.is_some() { break; }
                }
            }
            job.next = end;
            if job.next > self.last() || (self.direction > 0 && job.candidate.is_some()) || (self.direction < 0 && job.first > self.search_origin.0 && job.candidate.is_some()) {
                if let Some(anchor) = job.candidate.or(job.fallback) { self.cursor = anchor; self.status = if self.searching { format!("/{}", self.query) } else { String::new() }; }
                else { self.status = "No search match".into(); }
                return Some(Effect::Continue);
            }
            self.search = Some(job); return Some(Effect::Continue);
        }
        if self.status == "Reading history…" { self.status.clear(); }
        self.render();
        None
    }
}

