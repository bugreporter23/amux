//! Projections of one authoritative native grid. No ANSI or clipboard effects.
use std::io;

use serde::{Deserialize, Serialize};

use super::{Cell, Term, TermMode};
use super::cell::Flags;
use crate::grid::Dimensions;
use crate::index::{Column, Line, Point};

pub const MAX_CELLS: usize = 32768;
pub const MAX_BYTES: usize = 524288;

#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct Descriptor {
    pub epoch: [u64; 2],
    pub sequence: u64,
    pub oldest: u64,
    pub next: u64,
    pub columns: usize,
    pub lines: usize,
    pub alternate: bool,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Row {
    #[serde(rename = "runs", with = "cell_runs")]
    pub cells: Vec<Cell>,
}

impl Row {
    pub fn resident_bytes(&self) -> usize {
        self.cells.capacity() * std::mem::size_of::<Cell>() + self.cells.iter().map(Cell::amux_extra_bytes).sum::<usize>()
    }
}

pub(crate) fn encode_cells(cells: &[Cell]) -> Vec<(Cell, String)> {
    let mut runs: Vec<(Cell, String)> = Vec::new();
    for cell in cells {
        if let Some((template, text)) = runs.last_mut() {
            template.c = cell.c;
            let same_attributes = template == cell;
            template.c = ' ';
            if same_attributes { text.push(cell.c); continue; }
        }
        let mut template = cell.clone(); template.c = ' ';
        runs.push((template, cell.c.to_string()));
    }
    runs
}

mod cell_runs {
    use super::*;
    pub fn serialize<S: serde::Serializer>(cells: &[Cell], serializer: S) -> Result<S::Ok, S::Error> {
        encode_cells(cells).serialize(serializer)
    }
    pub fn deserialize<'de, D: serde::Deserializer<'de>>(deserializer: D) -> Result<Vec<Cell>, D::Error> {
        let runs = Vec::<(Cell, String)>::deserialize(deserializer)?;
        Ok(runs.into_iter().flat_map(|(template, text)| text.chars().map(|c| Cell { c, ..template.clone() }).collect::<Vec<_>>()).collect())
    }
}

#[derive(Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case", deny_unknown_fields)]
pub enum Read {
    Rows { epoch: [u64; 2], start: u64, end: u64, projection: String,
           #[serde(default)] whole: Option<[u64; 2]> },
    Marker { epoch: [u64; 2], bound: u64, start: u64, end: u64, origin: [u64; 2], direction: i8, marker: u8 },
}

impl<T> Term<T> {
    pub fn amux_clear_history(&mut self) {
        let primary = if self.mode.contains(TermMode::ALT_SCREEN) { &mut self.inactive_grid } else { &mut self.grid };
        primary.clear_history();
    }

    pub fn amux_capture(&self, history: usize) -> io::Result<String> {
        let history = history.min(self.grid.history_size());
        if (history + self.screen_lines()).saturating_mul(self.columns()) > 8 * 1024 * 1024 {
            return Err(io::Error::other("capture exceeds 8 MiB cell budget"));
        }
        let text = self.bounds_to_string(Point::new(Line(-(history as i32)), Column(0)),
            Point::new(Line(self.screen_lines() as i32 - 1), Column(self.columns() - 1)));
        if text.len() > 8 * 1024 * 1024 {
            return Err(io::Error::other("capture exceeds 8 MiB text budget"));
        }
        Ok(text)
    }

    pub fn history_descriptor(&self) -> Descriptor {
        let primary = if self.mode.contains(TermMode::ALT_SCREEN) { &self.inactive_grid } else { &self.grid };
        let next = primary.amux_next.max(primary.history_size() as u64);
        Descriptor { epoch: [primary.amux_epoch, self.amux_generation],
            sequence: self.amux_history.sequence,
            oldest: next - primary.history_size() as u64, next,
            columns: self.columns(), lines: self.screen_lines(),
            alternate: self.mode.contains(TermMode::ALT_SCREEN) }
    }

    pub fn history_read(&self, data: &[u8]) -> io::Result<Vec<u8>> {
        let request: Read = serde_json::from_slice(data).map_err(io::Error::other)?;
        let descriptor = self.history_descriptor();
        let primary = if descriptor.alternate { &self.inactive_grid } else { &self.grid };
        let validate = |epoch, start, end| -> io::Result<()> {
            if epoch != descriptor.epoch { return Err(io::Error::other("history epoch changed")); }
            if start < descriptor.oldest { return Err(io::Error::other("history was evicted")); }
            if start > end || end > descriptor.next { return Err(io::Error::other("invalid history range")); }
            Ok(())
        };
        let line = |id| Line((id as i64 - descriptor.next as i64) as i32);
        let value = match request {
            Read::Rows { epoch, start, end, projection, whole } => {
                validate(epoch, start, end)?;
                if let Some([a, b]) = whole { validate(epoch, a, b)?; }
                if end - start > 128 || (end - start) as usize * self.columns() > MAX_CELLS {
                    return Err(io::Error::other("history page exceeds cell budget"));
                }
                if !matches!(projection.as_str(), "text" | "cells") {
                    return Err(io::Error::other("unknown history projection"));
                }
                let rows: Vec<Row> = (start..end).map(|id| Row {
                    cells: primary[line(id)][..].iter().map(|cell| {
                        if projection == "text" { cell.amux_text_cell() } else { cell.clone() }
                    }).collect(),
                }).collect();
                serde_json::json!({"descriptor": descriptor, "start": start, "rows": rows})
            },
            Read::Marker { epoch, bound, start, end, origin, direction, marker } => {
                validate(epoch, descriptor.oldest, bound)?;
                validate(epoch, start, end)?;
                if end > bound || end - start > 128 || (end - start) as usize * self.columns() > MAX_CELLS {
                    return Err(io::Error::other("marker page exceeds cell budget"));
                }
                if !matches!(direction, -1 | 1) || !matches!(marker, b'A' | b'B' | b'C' | b'D') {
                    return Err(io::Error::other("invalid marker query"));
                }
                let mut found = None;
                for id in start..end {
                    for col in 0..self.columns() {
                        for found_marker in primary[line(id)][Column(col)].amux_markers() {
                            let anchor = [id, col as u64 + u64::from(found_marker.after)];
                            if found_marker.kind == marker
                                && ((direction < 0 && anchor < origin) || (direction > 0 && anchor > origin))
                            {
                                if direction < 0 || found.is_none() { found = Some(anchor); }
                            }
                        }
                    }
                    if direction > 0 && found.is_some() { break; }
                }
                serde_json::json!({"descriptor": descriptor, "anchor": found})
            },
        };
        let encoded = serde_json::to_vec(&value).map_err(io::Error::other)?;
        if encoded.len() > MAX_BYTES { return Err(io::Error::other("history page exceeds byte budget")); }
        Ok(encoded)
    }

    /// Resize, keeping the rows of a shell prompt under the cursor unjoined when
    /// widening. Shells redraw on SIGWINCH by moving up the row count the prompt
    /// had at the old width; joining its wrapped rows first would make that move
    /// erase the line above the prompt. Returns how rows moved, when both
    /// geometries are on the primary screen.
    pub fn amux_resize<S: Dimensions>(&mut self, size: S) -> Option<Reflow> {
        if size.columns() > self.columns() && !self.mode.contains(TermMode::ALT_SCREEN) {
            if let Some(start) = self.amux_prompt_start() {
                let last = Column(self.columns() - 1);
                for line in start..self.grid.cursor.point.line.0 {
                    self.grid[Line(line)][last].flags.remove(Flags::WRAPLINE);
                }
            }
        }
        let before = LineMap::capture(self);
        self.resize(size);
        let after = LineMap::capture(self);
        Some(Reflow { before: before?, after: after? }).filter(|reflow| reflow.before.epoch != reflow.after.epoch)
    }

    /// First row the shell redraws when the cursor is on prompt input: the A row
    /// with standard marks (A prompt, B input), or the C row opening a prelude
    /// that precedes A-marked input.
    fn amux_prompt_start(&self) -> Option<i32> {
        let top = -(self.grid.history_size() as i32);
        let last = Column(self.columns() - 1);
        let marked = |line: i32, kind: u8| self.grid[Line(line)][..].iter()
            .any(|cell| cell.amux_markers().iter().any(|marker| marker.kind == kind));
        let cursor = self.grid.cursor.point.line.0;
        let mut input = cursor;
        while input > top && self.grid[Line(input - 1)][last].flags.contains(Flags::WRAPLINE) { input -= 1; }
        let above = (top.max(input - self.screen_lines() as i32)..input).rev();
        if (input..=cursor).any(|line| marked(line, b'B')) {
            if marked(input, b'A') { return Some(input); }
            for line in above {
                if marked(line, b'A') { return Some(line); }
                if marked(line, b'C') || marked(line, b'D') { return None; }
            }
            None
        } else if marked(input, b'A') {
            for line in above {
                if marked(line, b'A') { break; }
                if marked(line, b'C') { return Some(line); }
            }
            Some(input)
        } else { None }
    }

    pub fn screen_rows(&self) -> Vec<Row> {
        (0..self.screen_lines()).map(|i| Row { cells: self.grid[Line(i as i32)][..].to_vec() }).collect()
    }
}

/// Logical-line structure of the primary screen and its history at one epoch.
/// Reflow preserves logical lines, so a position is carried across a resize as
/// its logical-line distance from the cursor's line plus a cell offset.
pub struct LineMap { epoch: [u64; 2], oldest: u64, columns: usize, wraps: Vec<bool>, cursor: usize }

pub struct Reflow { before: LineMap, after: LineMap }

impl LineMap {
    fn capture<T>(term: &Term<T>) -> Option<Self> {
        if term.mode.contains(TermMode::ALT_SCREEN) { return None; }
        let descriptor = term.history_descriptor();
        let top = -(term.grid.history_size() as i32);
        let last = Column(term.columns() - 1);
        let wraps = (top..term.screen_lines() as i32)
            .map(|line| term.grid[Line(line)][last].flags.contains(Flags::WRAPLINE)).collect();
        Some(Self { epoch: descriptor.epoch, oldest: descriptor.oldest, columns: term.columns(), wraps,
            cursor: (term.grid.cursor.point.line.0 - top) as usize })
    }

    fn start(&self, mut index: usize) -> usize {
        while index > 0 && self.wraps[index - 1] { index -= 1; }
        index
    }

    /// Logical lines that begin before `index`.
    fn logical(&self, index: usize) -> i64 {
        self.wraps[..self.start(index)].iter().filter(|wrap| !**wrap).count() as i64
    }

    fn anchor(&self, (row, column): (u64, usize)) -> Option<(i64, usize)> {
        let (index, column) = if row < self.oldest { (0, 0) } else { ((row - self.oldest) as usize, column) };
        if index >= self.wraps.len() { return None; }
        let start = self.start(index);
        Some((self.logical(self.cursor) - self.logical(index), (index - start) * self.columns + column.min(self.columns - 1)))
    }

    fn locate(&self, (distance, offset): (i64, usize)) -> (u64, usize) {
        let target = self.logical(self.cursor) - distance;
        if target < 0 { return (self.oldest, 0); }
        let mut line = 0;
        let mut start = None;
        for index in 0..self.wraps.len() {
            if index == 0 || !self.wraps[index - 1] {
                if line == target { start = Some(index); break; }
                line += 1;
            }
        }
        let Some(start) = start else { return (self.oldest + self.wraps.len() as u64 - 1, 0); };
        let mut end = start;
        while end + 1 < self.wraps.len() && self.wraps[end] { end += 1; }
        let index = start + offset / self.columns;
        if index > end { (self.oldest + end as u64, self.columns - 1) }
        else { (self.oldest + index as u64, offset % self.columns) }
    }
}

/// Recent reflows, so copy views can follow their positions across resizes.
#[derive(Default)]
pub struct Reflows(std::collections::VecDeque<Reflow>);

impl Reflows {
    const RETAINED: usize = 16;

    pub fn push(&mut self, reflow: Reflow) {
        if self.0.len() == Self::RETAINED { self.0.pop_front(); }
        self.0.push_back(reflow);
    }

    pub fn clear(&mut self) { self.0.clear(); }

    fn carry(&self, mut epoch: [u64; 2], mut point: (u64, usize), current: [u64; 2]) -> Option<(u64, usize)> {
        while epoch != current {
            let reflow = self.0.iter().find(|reflow| reflow.before.epoch == epoch)?;
            point = reflow.after.locate(reflow.before.anchor(point)?);
            epoch = reflow.after.epoch;
        }
        Some(point)
    }

    /// Answer a `map` history read, or return `None` for other reads.
    pub fn read<T>(&self, term: &Term<T>, data: &[u8]) -> Option<io::Result<Vec<u8>>> {
        #[derive(Deserialize)]
        #[serde(deny_unknown_fields)]
        struct Map { kind: String, epoch: [u64; 2], points: Vec<[u64; 2]> }
        let value: serde_json::Value = serde_json::from_slice(data).ok()?;
        if value.get("kind").and_then(serde_json::Value::as_str) != Some("map") { return None; }
        Some((|| {
            let request: Map = serde_json::from_value(value).map_err(io::Error::other)?;
            debug_assert_eq!(request.kind, "map");
            if request.points.len() > 8 { return Err(io::Error::other("too many map points")); }
            let descriptor = term.history_descriptor();
            let last = descriptor.next + descriptor.lines as u64 - 1;
            let points: Vec<_> = request.points.iter().map(|&[row, column]| {
                if descriptor.alternate { return None; }
                let (row, column) = self.carry(request.epoch, (row, column as usize), descriptor.epoch)?;
                Some(if row < descriptor.oldest { [descriptor.oldest, 0] }
                    else if row > last { [last, 0] }
                    else { [row, column.min(descriptor.columns - 1) as u64] })
            }).collect();
            serde_json::to_vec(&serde_json::json!({"descriptor": descriptor, "points": points})).map_err(io::Error::other)
        })())
    }
}

/// Reuse native extraction state across a streamed selection.
pub struct RowText { terminal: Term<crate::event::VoidListener> }

impl RowText {
    pub fn new(columns: usize, tabs: Option<&[bool]>) -> Self {
        let mut terminal = Term::new(Default::default(), &crate::term::checkpoint::Size(columns, 2), crate::event::VoidListener);
        if let Some(tabs) = tabs { terminal.tabs.tabs = tabs.to_vec(); }
        Self { terminal }
    }

    pub fn extract(&mut self, row: &Row, successor: Option<&Row>, start: usize, end: usize, include_wrapped_wide: bool) -> String {
        for (column, cell) in row.cells.iter().enumerate() { self.terminal.grid[Line(0)][Column(column)] = cell.clone(); }
        self.terminal.grid[Line(1)][Column(0)] = successor.map(|next| next.cells[0].clone()).unwrap_or_default();
        self.terminal.line_to_string(Line(0), Column(start)..Column(end), include_wrapped_wide)
    }
}

pub fn point(row: u64, column: usize, first: u64) -> Point { Point::new(Line((row - first) as i32), Column(column)) }

