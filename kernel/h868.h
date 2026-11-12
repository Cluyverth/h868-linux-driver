/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef H868_H
#define H868_H

#include <linux/i2c.h>
#include <media/dvb_frontend.h>

struct dvb_frontend *h868_frontend_attach(struct i2c_adapter *demod,
                                         struct i2c_adapter *tuner);

#endif
