//! Editor domain model and mutation logic.

pub mod error;
pub mod world;

pub use error::{CoreError, CoreResult};
pub use world::{
    HeightRange, LiquidLayer, LiquidVertex, ModelPlacement, TerrainChunk, TerrainLayer, TileCoord,
    WmoPlacement, WorldMap, WorldTile,
};
