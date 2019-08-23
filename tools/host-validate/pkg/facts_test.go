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
	"testing"
)

func testXMLFile(t *testing.T, filename string) {
	xml, err := ioutil.ReadFile(filename)
	if err != nil {
		t.Fatal(err)
	}

	doc := &FactList{}

	err = doc.Unmarshal(string(xml))
	if err != nil {
		t.Fatal(fmt.Errorf("Cannot parse %s: %s", filename, err))
	}

	newxml, err := doc.Marshal()
	if err != nil {
		t.Fatal(fmt.Errorf("Cannot format %s: %s", filename, err))
	}

	err = testCompareXML(filename, string(xml), newxml, nil, nil)
	if err != nil {
		t.Fatal(fmt.Errorf("Cannot roundtrip %s: %s", filename, err))
	}
}

func TestRoundTrip(t *testing.T) {
	testXMLFile(t, "../data/devices.xml")
}
