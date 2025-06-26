/**
  * This file is part of the hoverboard-sideboard-hack project.
  *
  * Copyright (C) 2020-2021 Emanuel FERU <aerdronix@gmail.com>
  *
  * This program is free software: you can redistribute it and/or modify
  * it under the terms of the GNU General Public License as published by
  * the Free Software Foundation, either version 3 of the License, or
  * (at your option) any later version.
  *
  * This program is distributed in the hope that it will be useful,
  * but WITHOUT ANY WARRANTY; without even the implied warranty of
  * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  * GNU General Public License for more details.
  *
  * You should have received a copy of the GNU General Public License
  * along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef I2C_IT_H
#define I2C_IT_H

#include "config.h"

#ifdef SELF_BALANCING_ENABLE

#include "gd32f1x0.h"
#include "config.h"

/* Interrupt function declarations */
void I2C0_EV_IRQHandler(void);   // handle I2C0 event interrupt request
void I2C0_ER_IRQHandler(void);   // handle I2C0 error interrupt request


#endif
#endif /* I2C_IT_H */

