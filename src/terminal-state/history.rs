//! Projections of one authoritative native grid. No ANSI or clipboard effects.
use std::io;

use serde::{Deserialize, Serialize};

use super::{Cell, Term, TermMode};
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

    pub fn screen_rows(&self) -> Vec<Row> {
        (0..self.screen_lines()).map(|i| Row { cells: self.grid[Line(i as i32)][..].to_vec() }).collect()
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

