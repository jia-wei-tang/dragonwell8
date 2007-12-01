/*
 * Copyright 2005-2007 Sun Microsystems, Inc.  All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 */

/*
 * @test
 * @bug 6341798
 * @summary Tests name generation for turkish locale
 * @author Sergey Malenkov
 */

import java.beans.XMLDecoder;
import java.io.ByteArrayInputStream;
import java.util.Locale;

import static java.util.Locale.ENGLISH;

public class Test6341798 {
    private static final Locale TURKISH = new Locale("tr");

    private static final String DATA
            = "<java>\n"
            + " <object class=\"TestTurkishLocale$DataBean\">\n"
            + "  <void property=\"illegal\">\n"
            + "   <boolean>true</boolean>\n"
            + "  </void>\n"
            + " </object>\n"
            + "</java> ";

    public static void main(String[] args) {
        test(ENGLISH, DATA.getBytes());
        test(TURKISH, DATA.getBytes());
    }

    private static void test(Locale locale, byte[] data) {
        Locale.setDefault(locale);
        System.out.println("locale = " + locale);

        XMLDecoder decoder = new XMLDecoder(new ByteArrayInputStream(data));
        System.out.println("object = " + decoder.readObject());
        decoder.close();
    }

    public static class DataBean {
        private boolean illegal;

        public boolean isIllegal() {
            return this.illegal;
        }

        public void setIllegal(boolean illegal) {
            this.illegal = illegal;
        }

        public String toString() {
            if (this.illegal) {
                return "property is set";
            }
            throw new Error("property is not set");
        }
    }
}
