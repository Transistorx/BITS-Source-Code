"""Material list and operator label rename (transport-neutral)."""

from pydantic import BaseModel, ConfigDict, Field
from sqlalchemy import select
from sqlalchemy.orm import Session

from ..models import Material
from .errors import ServiceError


class MaterialLabelIn(BaseModel):
    model_config = ConfigDict(extra="forbid")
    name: str = Field(min_length=1, max_length=64)


def read_materials(db: Session) -> list[dict]:
    rows = db.scalars(select(Material).order_by(Material.material_id))
    return [{"material_id": m.material_id, "name": m.name, "channel_id": m.channel_id,
             "pump_id": m.pump_id, "relay_id": m.relay_id, "scale_id": m.scale_id,
             "enabled": m.enabled} for m in rows]


def rename_material(db: Session, material_id: str, payload: MaterialLabelIn) -> dict:
    row = db.get(Material, material_id)
    if row is None:
        raise ServiceError("not_found", "material not found", 404)
    row.name = payload.name.strip()
    if not row.name:
        raise ServiceError("invalid", "material name cannot be blank", 422)
    db.commit()
    return {"material_id": row.material_id, "name": row.name}
