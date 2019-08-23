/*
 * This file is part of the libvirt project
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * Copyright (C) 2019 Red Hat, Inc.
 *
 */

package pkg

import (
	"fmt"
	"io/ioutil"
	"path"
	"strings"
	"testing"
)

func testYAMLFile(t *testing.T, filename string) {
	_, err := NewFactList(filename)
	if err != nil {
		t.Fatal(fmt.Errorf("Cannot parse %s: %s", filename, err))
	}
}

func TestParse(t *testing.T) {
	dir := path.Join("..", "rules")
	files, err := ioutil.ReadDir(dir)
	if err != nil {
		t.Fatal(fmt.Errorf("Cannot read %s: %s", dir, err))
	}
	for _, file := range files {
		if strings.HasSuffix(file.Name(), ".yaml") {
			testYAMLFile(t, path.Join(dir, file.Name()))
		}
	}
}
