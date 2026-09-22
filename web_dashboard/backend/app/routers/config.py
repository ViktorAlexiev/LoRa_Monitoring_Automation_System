from fastapi import APIRouter, Depends

from .. import config as config_module
from .. import schemas
from ..auth import get_current_user, require_role

router = APIRouter(prefix="/api/config", tags=["config"], dependencies=[Depends(get_current_user)])


@router.get("", response_model=schemas.SystemConfig)
def get_config():
    return config_module.CONFIG


@router.patch("", response_model=schemas.SystemConfig, dependencies=[Depends(require_role("admin"))])
def update_config(payload: schemas.SystemConfigUpdate):
    data = payload.model_dump(exclude_unset=True)
    for section, values in data.items():
        target = config_module.CONFIG.setdefault(section, {})
        for key, value in values.items():
            if value is not None:
                target[key] = value
    config_module.save()
    return config_module.CONFIG
