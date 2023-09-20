// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

package org.apache.doris.qe;

import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;

/**
 * Sql translator, convert db-link sql to catalog sql.
 */
public class DbLinkStmtTranslator {
    private static final Logger LOG = LogManager.getLogger(DbLinkStmtTranslator.class);

    public enum DbLinkOpType {
        UNKNOWN,
        CREATE, // create db-link
        DROP, // drop db-link
        // CHECKSTYLE OFF
        DML_SELECT,
        DML_INSERT,
        DML_UPDATE,
        DML_DELETE;

        public boolean isDML() {
            return this == DbLinkOpType.DML_SELECT || this == DbLinkOpType.DML_INSERT
                || this == DbLinkOpType.DML_UPDATE || this == DbLinkOpType.DML_DELETE;
        }
    }

    public String translateStmt(String originStmt) throws Exception {
        if (originStmt == null) {
            return null;
        }
        
        String finalStmt = null;
        DbLinkOpType type = checkType(originStmt);
        switch (type) {
            case CREATE:
            case DROP:
                finalStmt = translateDbLinkDdlStmt(originStmt, type);
                break;
            case DML_SELECT:
            case DML_INSERT:
            case DML_UPDATE:
            case DML_DELETE:
                finalStmt = translateDbLinkDmlStmt(originStmt, type);
            default:
                throw new Exception("not support op type " + type + " for db-link");
        }
        return finalStmt;
    }

    private DbLinkOpType checkType(String originStmt) {
        DbLinkOpType type = DbLinkOpType.UNKNOWN;
        return type;
    }

    private String translateDbLinkDdlStmt(String originStmt, DbLinkOpType type) {
        String finalStmt = null;
        return finalStmt;
    }

    private String translateDbLinkDmlStmt(String originStmt, DbLinkOpType type) {
        String finalStmt = null;
        return finalStmt;
    }
}
